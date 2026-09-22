// ResponsesApi.cpp - /v1/responses：完整事件流 + previous_response_id 本地会话缓存
#include "api/Facade.h"
#include "common/Crypto.h"
#include "common/Log.h"
#include "common/Settings.h"
#include "accounts/Storage.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

std::string makeId(const char* prefix) {
    return std::string(prefix) + "_" + crypto::sha512Hex(crypto::genUuid()).substr(0, 32);
}

// ---------- 会话缓存落盘（UTF-8 安全路径读写） ----------

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string readSessionFileUtf8(const std::string& path) {
    HANDLE h = CreateFileW(utf8ToWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
    CloseHandle(h);
    return out;
}

// tmp + MoveFileEx 原子替换（与 Config::save 同一模式），避免写一半崩溃留脏文件
void writeSessionFileUtf8Atomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    HANDLE h = CreateFileW(utf8ToWide(tmp).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    size_t off = 0;
    BOOL ok = TRUE;
    DWORD written = 0;
    while (ok && off < data.size()) {
        if (!WriteFile(h, data.data() + (DWORD)off, (DWORD)(data.size() - off), &written, nullptr))
            ok = FALSE;
        else
            off += written;
    }
    CloseHandle(h);
    if (ok && MoveFileExW(utf8ToWide(tmp).c_str(), utf8ToWide(path).c_str(), MOVEFILE_REPLACE_EXISTING))
        return;
    DeleteFileW(utf8ToWide(tmp).c_str());
}

std::string sanitizeToolName(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (isalnum((unsigned char)c) || c == '_' || c == '-') out += c;
        else out += '_';
    }
    if (out.empty()) out = "tool";
    if (out.size() > 64) out = out.substr(0, 54) + crypto::sha512Hex(in).substr(0, 10);
    return out;
}

// namespaced 工具用稳定内部名称上行；输出通过声明表恢复原名称和 namespace。
std::string upstreamToolName(const std::string& name, const std::string& nameSpace) {
    if (nameSpace.empty()) return sanitizeToolName(name);
    return sanitizeToolName(nameSpace).substr(0, 20) + "__" + sanitizeToolName(name).substr(0, 20) +
           "_" + crypto::sha512Hex(nameSpace + "\n" + name).substr(0, 12);
}

struct ResponseToolSpec {
    std::string name, nameSpace;
    bool custom = false;
};
using ResponseToolMap = std::map<std::string, ResponseToolSpec>;

// 会话缓存（previous_response_id → 本地重建上下文）
struct CachedSession {
    std::vector<ChatMessage> messages;
    long long ts = 0;
};

// ChatMessage ↔ 落盘 JSON。图片等多模态块不落盘（data URI 可达数百 MB）：
// 重启恢复的是纯文本 + 工具调用历史，content 是权威文本所以不丢上下文。
Json chatMessageToJson(const ChatMessage& m) {
    Json j = Json::object();
    j.set("role", Json(m.role));
    j.set("content", Json(m.content));
    if (!m.toolCallId.empty()) j.set("tool_call_id", Json(m.toolCallId));
    if (!m.toolName.empty()) j.set("name", Json(m.toolName));
    if (!m.toolCalls.empty()) {
        Json tcs = Json::array();
        for (auto& t : m.toolCalls) {
            Json tc = Json::object();
            tc.set("id", Json(t.id));
            tc.set("name", Json(t.name));
            tc.set("arguments", Json(t.arguments));
            tcs.push_back(tc);
        }
        j.set("tool_calls", tcs);
    }
    return j;
}

ChatMessage chatMessageFromJson(const Json& j) {
    ChatMessage m;
    m.role = j.get("role", Json("user")).asString();
    m.content = j.get("content", Json("")).asString();
    m.toolCallId = j.get("tool_call_id", Json("")).asString();
    m.toolName = j.get("name", Json("")).asString();
    const Json* tcs = j.find("tool_calls");
    if (tcs && tcs->isArray()) {
        for (size_t k = 0; k < tcs->size(); ++k) {
            const Json& tc = tcs->at(k);
            ToolCall t;
            t.id = tc.get("id", Json("")).asString();
            t.name = tc.get("name", Json("")).asString();
            t.arguments = tc.get("arguments", Json("")).asString();
            if (!t.name.empty()) m.toolCalls.push_back(t);
        }
    }
    return m;
}

struct SessionCache {
    std::mutex mtx;
    std::map<std::string, CachedSession> map;
    std::deque<std::string> order;
    bool loaded = false;

    static std::string path() { return exeDir() + "\\responses_sessions.json"; }

    // 惰性加载一次：persist 关闭时不读（保持纯内存语义）
    void ensureLoaded() {
        if (loaded) return;
        loaded = true;
        if (!settings::get()->responsesSessionCachePersist) return;
        std::string data = readSessionFileUtf8(path());
        if (data.empty()) return;
        Json root;
        if (!Json::parse(data, root) || !root.isArray()) {
            LOG_W("responses 会话缓存文件解析失败，忽略: %s", path().c_str());
            return;
        }
        for (size_t i = 0; i < root.size(); ++i) {
            const Json& e = root.at(i);
            std::string id = e.get("id", Json("")).asString();
            if (id.empty() || map.count(id)) continue;
            CachedSession cs;
            cs.ts = (long long)e.get("ts", Json((double)0)).asInt(0);
            const Json* ms = e.find("messages");
            if (ms && ms->isArray()) {
                for (size_t k = 0; k < ms->size(); ++k)
                    cs.messages.push_back(chatMessageFromJson(ms->at(k)));
            }
            if (!cs.messages.empty()) {
                map[id] = std::move(cs);
                order.push_back(id);
            }
        }
        LOG_I("responses 会话缓存已从磁盘恢复 %zu 条", map.size());
    }

    // 写透整份缓存（put 每响应至多一次，文本量级下开销可忽略）
    void persistLocked() {
        if (!settings::get()->responsesSessionCachePersist) return;
        Json root = Json::array();
        for (auto& id : order) {
            auto it = map.find(id);
            if (it == map.end()) continue;
            Json e = Json::object();
            e.set("id", Json(id));
            e.set("ts", Json(it->second.ts));
            Json ms = Json::array();
            for (auto& m : it->second.messages) ms.push_back(chatMessageToJson(m));
            e.set("messages", ms);
            root.push_back(e);
        }
        std::string data = root.dump();
        if (data.size() > 32ull * 1024 * 1024) {
            LOG_W("responses 会话缓存过大（%zu 字节），本次跳过落盘", data.size());
            return;
        }
        writeSessionFileUtf8Atomic(path(), data);
    }

    void put(const std::string& id, CachedSession s, size_t cap) {
        std::lock_guard<std::mutex> lk(mtx);
        ensureLoaded();
        if (!cap) cap = 64;
        if (map.find(id) == map.end()) {
            order.push_back(id);
            while (order.size() > cap) {
                map.erase(order.front());
                order.pop_front();
            }
        }
        s.ts = (long long)time(nullptr);
        map[id] = std::move(s);
        persistLocked();
    }
    bool get(const std::string& id, CachedSession& out) {
        std::lock_guard<std::mutex> lk(mtx);
        ensureLoaded();
        auto it = map.find(id);
        if (it == map.end()) return false;
        out = it->second;
        return true;
    }
};
SessionCache& cache() {
    static SessionCache c;
    return c;
}

// ---------- input 归一化 ----------
// 把 Responses content（字符串/块数组）归一到 ChatMessage：文本聚合 content，
// 图片进 parts（input_image 透传，不得省略）；input_file（PDF 等）暂不支持，
// 静默忽略，不写占位噪音。
void blocksToMessage(const Json& content, ChatMessage& m) {
    if (content.isString()) { m.content += content.asString(); return; }
    if (!content.isArray()) return;
    std::vector<MessagePart> parts;
    bool hasImage = false;
    for (size_t i = 0; i < content.size(); ++i) {
        const Json& b = content.at(i);
        if (b.isString()) {
            m.content += b.asString();
            MessagePart mp;
            mp.type = MessagePart::Text;
            mp.text = b.asString();
            parts.push_back(mp);
            continue;
        }
        if (!b.isObject()) continue;
        std::string t = b.get("type", Json("text")).asString();
        if (t == "input_text" || t == "output_text" || t == "text" || t == "refusal") {
            std::string tx = b.get("text", Json("")).asString();
            m.content += tx;
            MessagePart mp;
            mp.type = MessagePart::Text;
            mp.text = tx;
            parts.push_back(mp);
        } else if (t == "input_image" || t == "image") {
            std::string url;
            if (partImageUrl(b, url)) {
                MessagePart mp;
                mp.type = MessagePart::Image;
                mp.url = url;
                parts.push_back(mp);
                hasImage = true;
            }
        }
    }
    if (hasImage) m.parts = std::move(parts);
}

void normalizeInput(const Json& input, const std::string& instructions, bool replayReasoning,
                    std::vector<ChatMessage>& out) {
    if (!instructions.empty()) {
        ChatMessage sys;
        sys.role = "system";
        sys.content = instructions;
        out.push_back(sys);
    }
    // 思维链回放：上游没有 reasoning 槽位（encrypted_content 也不可解密转发），
    // 输入里 reasoning 项的可读摘要以 <think> 块并入其后第一条 assistant 消息，
    // 模型由此"记得"上一轮自己的推理；不回放则 Codex 式多轮等于每轮失忆。
    // 与 responsesMapReasoningSummary 同门控：关掉汇总即同时关掉回放。
    std::string pendingReason;
    bool adjacentAssistant = false;
    auto thinkPrefix = [](const std::string& t) {
        return "<think>\n" + t + "\n</think>\n\n";
    };
    auto pushItem = [&](const Json& item) {
        if (item.isString()) {
            adjacentAssistant = false;
            pendingReason.clear();
            ChatMessage m;
            m.role = "user";
            m.content = item.asString();
            out.push_back(m);
            return;
        }
        if (!item.isObject()) return;
        std::string type = item.get("type", Json("message")).asString();
        if (type == "message" || type == "easy_input_message") {
            std::string role = item.get("role", Json("user")).asString();
            if (role == "agent_message") role = "assistant";
            ChatMessage m;
            m.role = role;
            const Json* contentPtr = item.find("content");
            if (contentPtr) blocksToMessage(*contentPtr, m);
            if (role == "assistant" && !pendingReason.empty()) {
                // content 与 parts 都可能是权威序列化来源（soloContent 优先
                // parts），前缀必须并入实际会被上行的那一份
                if (m.parts.empty()) {
                    m.content = thinkPrefix(pendingReason) + m.content;
                } else {
                    for (auto& p : m.parts) {
                        if (p.type == MessagePart::Text) {
                            p.text = thinkPrefix(pendingReason) + p.text;
                            break;
                        }
                    }
                }
                pendingReason.clear();
            } else if (role != "assistant") {
                pendingReason.clear(); // 思维链锚不到 assistant：丢弃，避免错挂后文
            }
            adjacentAssistant = role == "assistant";
            // 空 assistant 也保留其轮次边界，后续调用只能挂到本轮。
            if (!m.content.empty() || !m.parts.empty() || adjacentAssistant) out.push_back(m);
        } else if (type == "function_call" || type == "custom_tool_call" || type == "tool_search_call") {
            ChatMessage* asst = nullptr;
            if (adjacentAssistant && !out.empty() && out.back().role == "assistant")
                asst = &out.back();
            if (!asst) {
                ChatMessage m;
                m.role = "assistant";
                if (!pendingReason.empty()) m.content = thinkPrefix(pendingReason);
                pendingReason.clear();
                out.push_back(m);
                asst = &out.back();
            } else {
                // 并入既有 assistant：本轮思维链已无处安放，丢弃防错挂
                pendingReason.clear();
            }
            ToolCall tc;
            tc.id = item.get("call_id", Json("")).asString();
            if (tc.id.empty()) tc.id = item.get("id", Json("")).asString();
            if (type == "custom_tool_call") {
                tc.name = item.get("name", Json("")).asString();
                const Json* inp = item.find("input");
                Json args = Json::object();
                args.set("input", inp && inp->isString() ? *inp : Json(inp ? inp->dump() : ""));
                tc.arguments = args.dump();
            } else if (type == "tool_search_call") {
                tc.name = "tool_search";
                tc.arguments = item.dump();
            } else {
                tc.name = item.get("name", Json("")).asString();
                const Json* ag = item.find("arguments");
                if (ag && ag->isString()) tc.arguments = ag->asString();
                else if (ag) tc.arguments = ag->dump();
                else tc.arguments = "{}";
            }
            if (!tc.name.empty()) {
                tc.name = upstreamToolName(tc.name, item.get("namespace", Json("")).asString());
                asst->toolCalls.push_back(tc);
            }
            adjacentAssistant = true;
        } else if (type == "function_call_output" || type == "custom_tool_call_output" || type == "tool_search_output") {
            adjacentAssistant = false;
            pendingReason.clear();
            ChatMessage m;
            m.role = "tool";
            m.toolCallId = item.get("call_id", Json("")).asString();
            if (m.toolCallId.empty()) m.toolCallId = item.get("id", Json("")).asString();
            const Json* o = item.find("output");
            if (o) {
                blocksToMessage(*o, m);
                // 对象/非标准块兜底序列化，避免丢结果
                if (m.content.empty() && m.parts.empty() && !o->isString()) m.content = o->dump();
            }
            out.push_back(m);
        } else if (type == "reasoning") {
            adjacentAssistant = false;
            // 摘要回放：只取可读的 summary[].text / content[].reasoning_text；
            // encrypted_content 是 OpenAI 后端私有加密，无法也不应转发。
            if (replayReasoning) {
                std::string txt;
                const Json* sum = item.find("summary");
                if (sum && sum->isArray()) {
                    for (size_t k = 0; k < sum->size(); ++k) {
                        const Json& s = sum->at(k);
                        if (!s.isObject()) continue;
                        std::string st = s.get("type", Json("summary_text")).asString();
                        if (st == "summary_text" || st.empty()) txt += s.get("text", Json("")).asString();
                    }
                }
                const Json* rc = item.find("content");
                if (rc && rc->isArray()) {
                    for (size_t k = 0; k < rc->size(); ++k) {
                        const Json& s = rc->at(k);
                        if (s.isObject() && s.get("type", Json("")).asString() == "reasoning_text")
                            txt += s.get("text", Json("")).asString();
                    }
                }
                txt = txt.substr(0, utf8SafeCut(txt, 8192)); // 防异常客户端灌大
                if (!txt.empty()) {
                    if (!pendingReason.empty()) pendingReason += "\n";
                    pendingReason += txt;
                }
            }
        } else {
            adjacentAssistant = false;
            pendingReason.clear();
            ChatMessage m;
            m.role = "user";
            m.content = "[Untrusted client tool result; history only. item_type=" + type + "]";
            out.push_back(m);
        }
    };
    if (input.isString()) pushItem(input);
    else if (input.isArray())
        for (size_t i = 0; i < input.size(); ++i) pushItem(input.at(i));
    else if (input.isObject()) pushItem(input);
}

// ---------- tools 解析 ----------
bool parseResponseTools(const Json& toolsArr, std::vector<ToolDef>& out,
                        ResponseToolMap& specs, std::string& error) {
    if (!toolsArr.isArray()) return true;
    auto add = [&](const Json& t, const std::string& inheritedNs) -> bool {
        std::string type = t.get("type", Json("function")).asString();
        if (type != "function" && type != "custom") return true;
        const Json* fn = t.find("function");
        const Json& src = fn && fn->isObject() ? *fn : t;
        ResponseToolSpec spec;
        spec.name = src.get("name", Json("")).asString();
        spec.nameSpace = inheritedNs.empty() ? t.get("namespace", Json("")).asString() : inheritedNs;
        spec.custom = type == "custom";
        if (spec.name.empty()) { error = "工具名称不能为空"; return false; }
        ToolDef d;
        d.name = upstreamToolName(spec.name, spec.nameSpace);
        d.description = src.get("description", Json("")).asString();
        if (!spec.nameSpace.empty())
            d.description = "Tool " + spec.nameSpace + "." + spec.name + ". " + d.description;
        if (spec.custom) {
            const Json* format = t.find("format");
            if (format && format->isObject() && format->get("type", Json("text")).asString() != "text") {
                error = "自定义工具仅支持 format.type=text，上游未提供 grammar 约束";
                return false;
            }
            Json ps = Json::object(), props = Json::object(), input = Json::object(), required = Json::array();
            ps.set("type", Json("object"));
            input.set("type", Json("string"));
            props.set("input", input);
            ps.set("properties", props);
            required.push_back(Json("input"));
            ps.set("required", required);
            ps.set("additionalProperties", Json(false));
            d.parameters = ps.dump();
        } else {
            const Json* ps = src.find("parameters");
            d.parameters = ps ? ps->dump() : "{}";
        }
        if (!specs.emplace(d.name, spec).second) {
            error = "工具名称重复或归一化后冲突: " + spec.name;
            return false;
        }
        out.push_back(std::move(d));
        return true;
    };
    for (size_t i = 0; i < toolsArr.size(); ++i) {
        const Json& t = toolsArr.at(i);
        if (!t.isObject()) continue;
        if (t.get("type", Json("function")).asString() == "namespace") {
            std::string ns = t.get("name", t.get("namespace", Json(""))).asString();
            if (ns.empty()) { error = "namespace 工具组必须指定 name"; return false; }
            const Json* subs = t.find("tools");
            if (subs && subs->isArray())
                for (size_t k = 0; k < subs->size(); ++k)
                    if (!add(subs->at(k), ns)) return false;
        } else if (!add(t, "")) return false;
    }
    return true;
}

// ---------- 流式输出状态 ----------
struct Emitter {
    HttpResponseWriter* w = nullptr;
    bool stream = false;
    int seq = 0;
    bool gone = false;

    bool emit(const char* type, Json extra) {
        if (!stream || gone || !w) return true;
        Json e = std::move(extra);
        e.set("type", Json(std::string(type)));
        e.set("sequence_number", Json(seq++));
        if (!w->writeSse(std::string(type), e.dump())) gone = true;
        return !gone;
    }
};

Json messageItem(const std::string& id, const std::string& text, const char* status) {
    Json item = Json::object();
    item.set("id", Json(id));
    item.set("type", Json("message"));
    item.set("role", Json("assistant"));
    item.set("status", Json(std::string(status)));
    Json content = Json::array();
    Json part = Json::object();
    part.set("type", Json("output_text"));
    part.set("annotations", Json::array());
    part.set("text", Json(text));
    content.push_back(part);
    item.set("content", content);
    return item;
}

Json responseObj(const std::string& id, const std::string& model, const char* status,
                 const Json& output, const Json& usage, bool parallelCalls = true) {
    Json r = Json::object();
    r.set("id", Json(id));
    r.set("object", Json("response"));
    r.set("created_at", Json((long long)time(nullptr)));
    r.set("status", Json(std::string(status)));
    r.set("background", Json(false));
    r.set("error", Json(nullptr));
    r.set("incomplete_details", Json(nullptr));
    r.set("output", output);
    r.set("parallel_tool_calls", Json(parallelCalls));
    r.set("service_tier", Json("default"));
    r.set("model", Json(model));
    Json u = usage.isObject() && usage.size() > 0 ? usage : Json::object();
    if (!u.find("input_tokens") && u.find("prompt_tokens")) {
        // OpenAI Responses 口径为 input/output_tokens;内部为 prompt/completion 命名,
        // 换名透传(含缓存与思考细分),否则落入下方零值兜底
        long long ptv = u.get("prompt_tokens", Json(0)).asInt(0);
        long long ctv = u.get("completion_tokens", Json(0)).asInt(0);
        long long ttv = u.get("total_tokens", Json(0)).asInt(0);
        if (ttv == 0) ttv = ptv + ctv;
        Json t = Json::object();
        t.set("input_tokens", Json(ptv));
        t.set("output_tokens", Json(ctv));
        t.set("total_tokens", Json(ttv));
        const Json* pd = u.find("prompt_tokens_details");
        if (pd && pd->isObject()) t.set("input_tokens_details", *pd);
        const Json* cd = u.find("completion_tokens_details");
        if (cd && cd->isObject()) t.set("output_tokens_details", *cd);
        u = t;
    }
    if (!u.find("input_tokens")) {
        u = Json::object();
        u.set("input_tokens", Json(0));
        u.set("output_tokens", Json(0));
        u.set("total_tokens", Json(0));
    }
    r.set("usage", u);
    return r;
}

} // namespace

void responsesHandle(const HttpRequest& req, HttpResponseWriter& w) {
    auto cfg = settings::get();
    Json errOut = Json::object();
    auto sendErr = [&](int status, const std::string& msg, const std::string& code = "") {
        Json e = Json::object();
        Json er = Json::object();
        er.set("message", Json(msg));
        er.set("type", Json(status == 503 ? "api_error" : "invalid_request_error"));
        if (!code.empty()) er.set("code", Json(code));
        e.set("error", er);
        w.respond(status, "application/json", e.dump());
    };
    if (!cfg->responsesEnabled) {
        sendErr(404, "Responses API 已禁用");
        return;
    }
    Json body;
    std::string perr;
    if (!Json::parse(req.body, body, &perr)) {
        sendErr(400, "请求体不是合法 JSON: " + perr);
        return;
    }
    bool stream = body.get("stream", Json(false)).asBool();
    std::string model = body.get("model", Json("auto")).asString();
    // background=true 需要后台任务 + /responses/{id} 轮询端点，本服务不支持；
    // 静默忽略会让轮询型客户端永远等不到结果，显式拒绝更友好。
    const Json* bg = body.find("background");
    if (bg && bg->isBool() && bg->asBool()) {
        sendErr(400, "不支持 background=true：本服务没有后台任务与响应查询端点",
                "background_not_supported");
        return;
    }
    const Json* ins = body.find("instructions");
    std::string instructions = ins && ins->isString() ? ins->asString() : "";
    const Json* inputPtr = body.find("input");
    const Json& input = inputPtr ? *inputPtr : Json();

    std::vector<ChatMessage> messages;
    std::string prevId = body.get("previous_response_id", Json("")).asString();
    if (!prevId.empty()) {
        CachedSession cs;
        if (!cache().get(prevId, cs)) {
            sendErr(400, "previous_response_id 不存在或已失效，请重新发送完整上下文",
                    "previous_response_not_found");
            return;
        }
        messages = std::move(cs.messages);
    }
    // input 表示新增历史，不按文本前缀猜测去重；本轮 instructions 不进入历史缓存。
    normalizeInput(input, "", cfg->responsesMapReasoningSummary, messages);
    std::vector<ChatMessage> upstreamMessages = messages;
    if (!instructions.empty()) {
        ChatMessage sys;
        sys.role = "system";
        sys.content = instructions;
        upstreamMessages.insert(upstreamMessages.begin(), std::move(sys));
    }

    std::vector<ToolDef> tools;
    ResponseToolMap toolSpecs;
    if (!parseResponseTools(body.get("tools", Json()), tools, toolSpecs, perr)) {
        sendErr(400, perr, "unsupported_or_invalid_tool");
        return;
    }
    // Responses 的 tool_choice 与 Chat Completions 同样需要在本地
    // 归一化：none 必须彻底移除工具；指定 function 时只保留目标。
    const Json* responseChoice = body.find("tool_choice");
    if (responseChoice && responseChoice->isString() && responseChoice->asString() == "none") {
        tools.clear();
    } else if (responseChoice && responseChoice->isObject()) {
        // 两种风格都认：嵌套 {"function":{"name":...}} 与扁平
        // {"type":"function","name":...}——只认一种会让另一种静默不匹配
        const Json* fn = responseChoice->find("function");
        std::string selected;
        if (fn && fn->isObject()) {
            selected = fn->get("name", Json("")).asString();
        } else {
            std::string ty = responseChoice->get("type", Json("")).asString();
            if (ty == "function" || ty == "custom" || ty.empty()) selected = responseChoice->get("name", Json("")).asString();
        }
        if (!selected.empty()) {
            // 与声明和历史使用同一名称映射，避免 namespaced 工具无法匹配。
            selected = upstreamToolName(selected, responseChoice->get("namespace", Json("")).asString());
            tools.erase(std::remove_if(tools.begin(), tools.end(), [&](const ToolDef& t) {
                return t.name != selected;
            }), tools.end());
        }
    }
    // parallel_tool_calls 按 OpenAI 语义只是给模型的提示，不丢弃任何调用；
    // 请求值原样回显到响应对象。
    bool parallelCalls = true;
    const Json* ptc = body.find("parallel_tool_calls");
    if (ptc && ptc->isBool()) parallelCalls = ptc->asBool();

    // reasoning.effort → R5
    std::string effortOverride;
    const Json* rea = body.find("reasoning");
    if (rea && rea->isObject()) {
        std::string e = rea->get("effort", Json("")).asString();
        if (!e.empty() && e != "none" && e != "off") effortOverride = e;
        // none/off → 不覆盖
    }

    auto acc = AccountPool::instance().acquire(30000);
    // 使用记录计时：从占用账号起算
    const auto usageStart = std::chrono::steady_clock::now();
    if (!acc) {
        sendErr(503, AccountPool::instance().hasUsableAccount() ? "账号并发繁忙，请稍后重试" : "无可用账号");
        return;
    }
    AccountPool::instance().ensureFreshToken(*acc);
    ModelCaps caps;
    ResolvedSettings rs;
    if (!resolveSettings(*acc, model, effortOverride, -1, caps, rs, perr)) {
        AccountPool::instance().release(acc, false, 0);
        sendErr(400, perr);
        return;
    }

    std::string respId = makeId("resp");
    std::string modelName = rs.displayName.empty() ? rs.configName : rs.displayName;
    const long long createdAt = (long long)time(nullptr);
    auto makeResponse = [&](const char* status, const Json& output, const Json& usage) {
        Json value = responseObj(respId, modelName, status, output, usage, parallelCalls);
        value.set("created_at", Json(createdAt));
        value.set("previous_response_id", prevId.empty() ? Json(nullptr) : Json(prevId));
        value.set("instructions", instructions.empty() ? Json(nullptr) : Json(instructions));
        value.set("tools", body.get("tools", Json::array()));
        return value;
    };
    Emitter em;
    em.w = &w;
    em.stream = stream;
    if (stream && !w.beginStream(200, "text/event-stream")) {
        AccountPool::instance().release(acc, true, 0);
        return;
    }

    // Responses 流必须先发送生命周期事件。部分 SDK 依赖 created /
    // in_progress 建立 response 对象，不能直接从 output_item.added 开始。
    if (stream) {
        Json lifecycle = makeResponse("in_progress", Json::array(), Json());
        lifecycle.set("usage", Json(nullptr));
        Json created = Json::object();
        created.set("response", lifecycle);
        if (!em.emit("response.created", created)) {
            AccountPool::instance().release(acc, true, 0);
            w.endStream();
            return;
        }
        Json progress = Json::object();
        progress.set("response", lifecycle);
        if (!em.emit("response.in_progress", progress)) {
            AccountPool::instance().release(acc, true, 0);
            w.endStream();
            return;
        }
    }

    // output 数组在条目首次出现时分配索引和 ID，增量事件与最终响应共用同一份条目。
    Json output = Json::array();
    int msgIdx = -1, rsIdx = -1;
    std::string allText, allReason, conversionError;
    ToolAccumulator tac;
    auto emitAdded = [&](int index) {
        Json ex = Json::object();
        ex.set("output_index", Json(index));
        ex.set("item", output.at(index));
        return em.emit("response.output_item.added", std::move(ex));
    };
    auto itemEvent = [&](int index) {
        Json ex = Json::object();
        ex.set("item_id", output.at(index).get("id", Json("")));
        ex.set("output_index", Json(index));
        return ex;
    };
    UpResult r = runChatPipeline(
        *acc, caps, rs, upstreamMessages, tools,
        [&](const UpEvent& e) -> bool {
            if (e.type == UpEvent::Reason) {
                allReason += e.text;
                if (!cfg->responsesMapReasoningSummary) return true;
                if (rsIdx < 0) {
                    rsIdx = (int)output.size();
                    Json item = Json::object();
                    item.set("id", Json(makeId("rs")));
                    item.set("type", Json("reasoning"));
                    item.set("summary", Json::array());
                    item.set("status", Json("in_progress"));
                    output.push_back(item);
                    if (!emitAdded(rsIdx)) return false;
                    Json part = Json::object();
                    part.set("type", Json("summary_text"));
                    part.set("text", Json(""));
                    Json ex = itemEvent(rsIdx);
                    ex.set("summary_index", Json(0));
                    ex.set("part", part);
                    if (!em.emit("response.reasoning_summary_part.added", std::move(ex))) return false;
                }
                Json ex = itemEvent(rsIdx);
                ex.set("summary_index", Json(0));
                ex.set("delta", Json(e.text));
                return em.emit("response.reasoning_summary_text.delta", std::move(ex));
            }
            if (e.type == UpEvent::Text) {
                allText += e.text;
                if (msgIdx < 0) {
                    msgIdx = (int)output.size();
                    Json item = messageItem(makeId("msg"), "", "in_progress");
                    item.set("content", Json::array());
                    output.push_back(item);
                    if (!emitAdded(msgIdx)) return false;
                    Json part = Json::object();
                    part.set("type", Json("output_text"));
                    part.set("text", Json(""));
                    part.set("annotations", Json::array());
                    Json ex = itemEvent(msgIdx);
                    ex.set("content_index", Json(0));
                    ex.set("part", part);
                    if (!em.emit("response.content_part.added", std::move(ex))) return false;
                }
                Json ex = itemEvent(msgIdx);
                ex.set("content_index", Json(0));
                ex.set("delta", Json(e.text));
                ex.set("logprobs", Json::array());
                return em.emit("response.output_text.delta", std::move(ex));
            }
            if (e.type == UpEvent::ToolCall) {
                // 上游事件已经包含完整参数，custom 的 JSON 包装只在上游一侧存在。
                auto spec = toolSpecs.find(e.toolName);
                bool custom = spec != toolSpecs.end() && spec->second.custom;
                std::string value = e.toolArgs.empty() ? "{}" : e.toolArgs;
                if (custom) {
                    Json args;
                    if (!Json::parse(value, args) || !args.find("input") || !args.find("input")->isString()) {
                        conversionError = "上游自定义工具参数缺少字符串 input: " + spec->second.name;
                        return false;
                    }
                    value = args.find("input")->asString();
                }
                tac.onEvent(e);
                int index = (int)output.size();
                Json item = Json::object();
                item.set("id", Json(makeId(custom ? "ctc" : "fc")));
                item.set("type", Json(custom ? "custom_tool_call" : "function_call"));
                item.set("call_id", Json(e.toolId));
                item.set("name", Json(spec == toolSpecs.end() ? e.toolName : spec->second.name));
                if (spec != toolSpecs.end() && !spec->second.nameSpace.empty())
                    item.set("namespace", Json(spec->second.nameSpace));
                item.set(custom ? "input" : "arguments", Json(""));
                item.set("status", Json("in_progress"));
                output.push_back(item);
                if (!emitAdded(index)) return false;
                Json ex = itemEvent(index);
                ex.set("delta", Json(value));
                output.at(index).set(custom ? "input" : "arguments", Json(value));
                return em.emit(custom ? "response.custom_tool_call_input.delta" : "response.function_call_arguments.delta",
                               std::move(ex));
            }
            return true;
        });
    if (!conversionError.empty()) {
        r.ok = false;
        r.clientAborted = false;
        r.error = conversionError;
        r.code = 0;
    }
    const bool incomplete = r.finishReason == "length" || r.finishReason == "content_filter";
    logRequestFailure("responses", caps.configName, *acc, r);
    const char* itemStatus = r.ok && !incomplete ? "completed" : "incomplete";
    auto toolResults = tac.take();

    // 更新聚合内容并关闭条目；终止原因决定 completed / incomplete，不能吞掉截断状态。
    for (size_t i = 0; i < output.size(); ++i) {
        Json& item = output.at(i);
        std::string type = item.get("type", Json("")).asString();
        item.set("status", Json(itemStatus));
        Json ex = itemEvent((int)i);
        if (type == "message") {
            item = messageItem(item.get("id", Json("")).asString(), allText, itemStatus);
            ex.set("content_index", Json(0));
            ex.set("text", Json(allText));
            if (r.ok) em.emit("response.output_text.done", ex);
            ex.erase("text");
            ex.set("part", item.get("content", Json::array()).at(0));
            if (r.ok) em.emit("response.content_part.done", ex);
        } else if (type == "reasoning") {
            Json summary = Json::array(), part = Json::object();
            part.set("type", Json("summary_text"));
            part.set("text", Json(allReason));
            summary.push_back(part);
            item.set("summary", summary);
            ex.set("summary_index", Json(0));
            ex.set("text", Json(allReason));
            if (r.ok) em.emit("response.reasoning_summary_text.done", ex);
            ex.erase("text");
            ex.set("part", part);
            if (r.ok) em.emit("response.reasoning_summary_part.done", ex);
        } else {
            bool custom = type == "custom_tool_call";
            ex.set(custom ? "input" : "arguments", item.get(custom ? "input" : "arguments", Json("")));
            if (!custom) ex.set("name", item.get("name", Json("")));
            if (r.ok) em.emit(custom ? "response.custom_tool_call_input.done" : "response.function_call_arguments.done", ex);
        }
        if (r.ok) {
            Json done = Json::object();
            done.set("output_index", Json((int)i));
            done.set("item", item);
            em.emit("response.output_item.done", std::move(done));
        }
    }

    // 一轮 assistant 的正文和所有并行调用保留在同一条历史消息中。
    // 先提交缓存再发送终止事件，收到 completed 的客户端可以立即续接。
    if (r.ok && !r.clientAborted && !em.gone && !incomplete) {
        CachedSession cs;
        cs.messages = messages;
        ChatMessage assistant;
        assistant.role = "assistant";
        if (cfg->responsesMapReasoningSummary && !allReason.empty())
            assistant.content = "<think>\n" + allReason + "\n</think>\n\n";
        assistant.content += allText;
        for (auto& t : toolResults)
            assistant.toolCalls.push_back({t.id, t.name, t.args});
        if (!assistant.content.empty() || !assistant.toolCalls.empty())
            cs.messages.push_back(std::move(assistant));
        cache().put(respId, std::move(cs), (size_t)cfg->responsesSessionCacheSize);
    }
    const char* responseStatus = !r.ok ? "failed" : incomplete ? "incomplete" : "completed";
    Json finalResponse = makeResponse(responseStatus, output, r.usage);
    if (incomplete) {
        Json details = Json::object();
        details.set("reason", Json(r.finishReason == "length" ? "max_output_tokens" : "content_filter"));
        finalResponse.set("incomplete_details", details);
    }
    if (!r.ok) {
        Json error = Json::object();
        error.set("code", Json(r.code ? std::to_string(r.code) : "upstream_error"));
        error.set("message", Json(r.error.empty() ? "上游请求失败" : r.error));
        finalResponse.set("error", error);
    }
    if (em.stream) {
        if (!em.gone && !r.clientAborted) {
            Json ex = Json::object();
            ex.set("response", finalResponse);
            em.emit(!r.ok ? "response.failed" : incomplete ? "response.incomplete" : "response.completed", std::move(ex));
        }
        w.endStream();
    }

    AccountPool::instance().release(acc, r.ok || r.clientAborted, r.code);
    // 消耗不随聊天流下发（2026-09-19 探针实测），请求后异步补查积分近实时更新
    if (r.ok || r.clientAborted) {
        // 使用记录登记：token 来自上游 token_usage，积分差值由积分回填时落账
        long long inT = 0, outT = 0, cacheT = 0;
        if (r.usage.isObject()) {
            inT = (long long)r.usage.get("prompt_tokens", Json((double)0)).asDouble(0);
            outT = (long long)r.usage.get("completion_tokens", Json((double)0)).asDouble(0);
            const Json* pd = r.usage.find("prompt_tokens_details");
            if (pd && pd->isObject())
                cacheT = (long long)pd->get("cached_tokens", Json((double)0)).asDouble(0);
        }
        if (inT || outT) {
            int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - usageStart).count();
            AccountPool::instance().usageRecordPending(acc, caps, "/v1/responses", inT, outT, cacheT, ms);
        }
        AccountPool::instance().refreshCreditsAsync(acc);
    }

    if (em.stream) return;
    if (!r.ok && !r.clientAborted) {
        // 非流式失败：标准 error envelope + 正确状态码，不返回 200/failed 对象
        int httpStatus = 502;
        std::string errType = "api_error";
        upstreamErrorType(r, httpStatus, errType);
        Json e = Json::object();
        Json er = Json::object();
        er.set("message", Json(r.error.empty() ? "上游请求失败" : r.error));
        er.set("type", Json(errType));
        if (r.code != 0) er.set("code", Json(std::to_string(r.code)));
        e.set("error", er);
        w.respond(httpStatus, "application/json", e.dump());
        return;
    }
    w.respond(200, "application/json",
              finalResponse.dump());
}
