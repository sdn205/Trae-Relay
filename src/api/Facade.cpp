// Facade.cpp - 路由与 OpenAI 兼容实现（chat completions / responses）
#include "api/Facade.h"
#include "common/Crypto.h"
#include "common/Log.h"
#include "upstream/ReasoningEffort.h"
#include "common/Settings.h"
#include <chrono>
#include <cmath>
#include <ctime>

// ================= 设置解析 =================

// 解析模型名（含 alias / display_name / 大小写）为 config_name
static bool resolveConfigName(Account& acc, const std::string& requested, ModelCaps& caps, std::string& err) {
    auto cfg = settings::get();
    const ModelConfig* mc = cfg->modelConfig(requested);
    std::string want = mc ? mc->name : requested;
    if (ModelCatalog::instance().get(acc, want, caps, err)) return true;
    // display_name / requested 兜底匹配
    err = "模型不存在或账号不可用: " + requested;
    return false;
}

bool resolveSettings(Account& acc, const std::string& requested, const std::string& effortOverride,
                     int maxOverride, ModelCaps& caps, ResolvedSettings& rs, std::string& err) {
    auto cfg = settings::get();
    if (!resolveConfigName(acc, requested, caps, err)) return false;
    if (!caps.present) {
        err = "模型不在账号模型表中: " + requested;
        return false;
    }
    const ModelConfig* mc = cfg->modelConfig(caps.configName);
    rs.requested = requested;
    rs.configName = caps.configName;
    rs.displayName = (mc && !mc->displayName.empty()) ? mc->displayName : caps.displayName;

    // ===== R5 思考强度 =====
    // 固定 solo 通道：档位只通过 system 前缀注入生效（见 UpstreamSolo.cpp），
    // 不再有"原生字段/前缀"写入方式选择。
    std::string wantEffort = effortOverride;
    if (wantEffort.empty() && mc && !mc->reasoningEffort.empty()) wantEffort = mc->reasoningEffort;
    if (wantEffort.empty() && (!mc || mc->reasoningEffort.empty())) wantEffort = cfg->defaultReasoningEffort;
    rs.effortWant = wantEffort;
    rs.effort = clampEffort(caps, wantEffort);
    if (rs.effort.empty() && !wantEffort.empty()) {
        LOG_W("R5 未生效：模型 %s 不支持档位 %s（模型表 options 为空或不匹配）",
              caps.configName.c_str(), wantEffort.c_str());
    }

    // ===== R6 Max =====
    int wantMax = maxOverride >= 0 ? maxOverride : 0;
    if (maxOverride < 0) {
        wantMax = mc ? mc->isMaxMode : cfg->defaultIsMaxMode;
        if (wantMax == 0 && (!mc || !mc->present)) wantMax = cfg->defaultIsMaxMode;
    }
    long long wantWindow = mc ? mc->maxContextWindow : 0;
    if (!wantWindow) wantWindow = cfg->defaultMaxContextWindow;
    rs.maxMode = clampMaxMode(caps, wantMax != 0, wantWindow, rs.window);
    return true;
}

// ================= 工具累积 =================

ToolAcc* ToolAccumulator::find(int idx) {
    for (auto& t : m_tools)
        if (t.index == idx) return &t;
    m_tools.push_back({ idx, "", "", "" });
    return &m_tools.back();
}

void ToolAccumulator::onEvent(const UpEvent& e) {
    if (e.type != UpEvent::ToolCall) return;
    ToolAcc* t = find(e.toolIndex >= 0 ? e.toolIndex : (int)m_tools.size());
    if (!t->id.empty() || !t->name.empty()) {
        // 新调用（同 index 复用被上游重置）：id 变化时新开条目
        if (!e.toolId.empty() && !t->id.empty() && e.toolId != t->id) {
            m_tools.push_back({ (int)m_tools.size(), e.toolId, e.toolName, e.toolArgs });
            return;
        }
    }
    if (!e.toolId.empty()) t->id = e.toolId;
    if (!e.toolName.empty()) t->name = e.toolName;
    if (!e.toolArgs.empty()) t->args += e.toolArgs;
}

namespace {

// stop 序列本地过滤器：上游协议没有 stop 字段，命中后由反代截断。扣住不足
// 一个最长 stop 的尾巴防跨帧命中；命中后其后正文全部丢弃（上游无法中途停止，
// 继续消费至自然结束以拿到完整 usage）。只作用于公开正文，不碰 reasoning 与
// 工具参数。按字节匹配是安全的：UTF-8 自同步，子串字节匹配即精确匹配。
class StopFilter {
public:
    void setStops(const std::vector<std::string>* s) {
        stops_ = s;
        holdMax_ = 0;
        for (auto& v : *s)
            if (v.size() > holdMax_) holdMax_ = v.size();
        if (holdMax_) holdMax_ -= 1;
    }
    bool hit() const { return hit_; }
    std::string feed(const std::string& t) {
        if (hit_) return "";
        if (!stops_ || stops_->empty()) return t;
        pend_ += t;
        size_t best = std::string::npos;
        for (auto& s : *stops_) {
            size_t p = pend_.find(s);
            if (p != std::string::npos && p < best) best = p;
        }
        if (best != std::string::npos) {
            hit_ = true;
            std::string out = pend_.substr(0, best);
            pend_.clear();
            return out;
        }
        if (pend_.size() > holdMax_) {
            size_t n = utf8SafeCut(pend_, pend_.size() - holdMax_);
            std::string out = pend_.substr(0, n);
            pend_.erase(0, n);
            return out;
        }
        return "";
    }
    // 流末冲刷：未成命中的残余前缀按普通正文放出
    std::string flush() {
        std::string out = std::move(pend_);
        pend_.clear();
        return out;
    }

private:
    const std::vector<std::string>* stops_ = nullptr;
    std::string pend_;
    size_t holdMax_ = 0;
    bool hit_ = false;
};

} // namespace

void normalizeOpenAiTools(const Json& toolsArr, const Json& toolChoice, std::vector<ToolDef>& out) {
    if (!toolsArr.isArray()) return;
    for (size_t i = 0; i < toolsArr.size(); ++i) {
        const Json& t = toolsArr.at(i);
        if (!t.isObject()) continue;
        std::string type = t.get("type", Json("function")).asString();
        if (type != "function") continue;
        const Json* fn = t.find("function");
        ToolDef d;
        if (fn && fn->isObject()) {
            d.name = fn->get("name", Json("")).asString();
            d.description = fn->get("description", Json("")).asString();
            const Json* ps = fn->find("parameters");
            // 规则 1：上游 parameters 为字符串 → 序列化
            d.parameters = ps ? ps->dump() : "{}";
        } else {
            // 扁平式（Responses）
            d.name = t.get("name", Json("")).asString();
            d.description = t.get("description", Json("")).asString();
            const Json* ps = t.find("parameters");
            d.parameters = ps ? ps->dump() : "{}";
        }
        if (!d.name.empty()) out.push_back(d);
    }
    // 规则 3：tool_choice 改写
    if (toolChoice.isString()) {
        std::string v = toolChoice.asString();
        if (v == "none") out.clear();
        // auto / required → 保留全部
    } else if (toolChoice.isObject()) {
        // 两种风格都认：chat 嵌套 {"function":{"name":...}} 与 Responses 扁平
        // {"type":"function","name":...}——只认一种会让另一种静默不匹配
        const Json* fn = toolChoice.find("function");
        std::string name;
        if (fn && fn->isObject()) {
            name = fn->get("name", Json("")).asString();
        } else {
            std::string ty = toolChoice.get("type", Json("")).asString();
            if (ty == "function" || ty.empty()) name = toolChoice.get("name", Json("")).asString();
        }
        if (!name.empty()) {
            std::vector<ToolDef> only;
            for (auto& d : out)
                if (d.name == name) only.push_back(d);
            out = only;
        }
    }
}

// ================= 通用管线 =================

// 上游错误码 → 下游 HTTP 状态与错误类型（chat / responses 共用）
void upstreamErrorType(const UpResult& r, int& httpStatus, std::string& type) {
    httpStatus = 502;
    type = "api_error";
    if (r.code == 1001 || r.httpStatus == 401) { httpStatus = 401; type = "authentication_error"; }
    else if (r.code == 1005) { httpStatus = 403; type = "insufficient_quota"; }
    else if (r.code == 429 || r.code == 4008 || r.code == 4011 || r.httpStatus == 429) {
        httpStatus = 429; type = "rate_limit_error";
    }
    else if (r.code == 4001) { httpStatus = 400; type = "invalid_request_error"; }
}

UpResult runChatPipeline(Account& acc, ModelCaps& caps, const ResolvedSettings& rs,
                         std::vector<ChatMessage> messages, std::vector<ToolDef> tools,
                         const UpSink& sink) {
    // 单轮上游请求：所有工具（含 web_search 等任何函数名）都作为普通函数
    // 透传给上游；模型返回 tool_calls 后本轮即结束，交给客户端执行，
    // 反代不拦截、不本地执行、不自动续跑。
    UpRequest rq;
    rq.model = caps.configName;
    rq.messages = std::move(messages);
    rq.tools = std::move(tools);
    rq.stream = true;
    rq.reasoningEffort = rs.effort;
    rq.maxMode = rs.maxMode;
    rq.maxContextWindow = rs.window;

    // 原生 function calling 已由上游归一成完整 ToolCall 事件。
    // 正文与思考中的协议示例属于普通内容，不能据此猜测或执行工具调用。
    int toolSeq = 0;
    UpResult r = upstreamDispatch(rq, acc, caps, [&](const UpEvent& e) -> bool {
        if (e.type == UpEvent::ToolCall) {
            UpEvent ne = e;
            ne.toolIndex = toolSeq++;
            if (ne.toolId.empty()) ne.toolId = "call_" + crypto::genUuid();
            if (ne.toolArgs.empty()) ne.toolArgs = "{}";
            return sink(ne);
        }
        return sink(e);
    });
    return r;
}

// ================= 路由 =================

static bool checkAuth(const HttpRequest& req) {
    auto cfg = settings::get();
    if (cfg->allowAnyApiKey) return true;
    if (cfg->apiKey.empty()) return false;
    const std::string* auth = req.header("authorization");
    if (auth) {
        std::string v = *auth;
        if (v.rfind("Bearer ", 0) == 0) v = v.substr(7);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        if (v == cfg->apiKey) return true;
    }
    const std::string* ak = req.header("x-api-key");
    if (ak && *ak == cfg->apiKey) return true;
    return false;
}

static void sendJsonError(HttpResponseWriter& w, int status, const std::string& type,
                          const std::string& message, const std::string& code = "") {
    Json e = Json::object();
    Json err = Json::object();
    err.set("message", Json(message));
    err.set("type", Json(type));
    if (!code.empty()) err.set("code", Json(code));
    e.set("error", err);
    w.respond(status, "application/json", e.dump());
}

static Json usageJson(const Json& u) {
    Json out = Json::object();
    long long pt = u.get("prompt_tokens", Json((double)0)).asInt(0);
    long long ct = u.get("completion_tokens", Json((double)0)).asInt(0);
    if (u.find("prompt_tokens") == nullptr) {
        pt = u.get("input_tokens", Json((double)0)).asInt(0);
        ct = u.get("output_tokens", Json((double)0)).asInt(0);
    }
    out.set("prompt_tokens", Json(pt));
    out.set("completion_tokens", Json(ct));
    out.set("total_tokens", Json(pt + ct));
    // 细分字段透传(上游 token_usage 平铺 cache/reasoning token,UpstreamSolo 已组装为 details)
    const Json* pd = u.find("prompt_tokens_details");
    if (pd && pd->isObject()) out.set("prompt_tokens_details", *pd);
    const Json* cd = u.find("completion_tokens_details");
    if (cd && cd->isObject()) out.set("completion_tokens_details", *cd);
    return out;
}

// ---------- /v1/chat/completions ----------

static void handleChatCompletions(const HttpRequest& req, HttpResponseWriter& w) {
    Json body;
    std::string perr;
    if (!Json::parse(req.body, body, &perr)) {
        sendJsonError(w, 400, "invalid_request_error", "请求体不是合法 JSON: " + perr);
        return;
    }
    std::string model = body.get("model", Json("auto")).asString();
    bool stream = body.get("stream", Json(settings::get()->defaultStream)).asBool();

    // 账号
    auto acc = AccountPool::instance().acquire(30000);
    if (!acc) {
        bool usable = AccountPool::instance().hasUsableAccount();
        sendJsonError(w, 503, "api_error",
                      usable ? "账号并发繁忙，请稍后重试" : "无可用账号（未发现凭证）");
        return;
    }
    LOG_D("chat: 账号已获取 %s", acc->nickname.c_str());
    AccountPool::instance().ensureFreshToken(*acc);
    ModelCaps caps;
    ResolvedSettings rs;
    // R5 请求级覆盖
    std::string effortOverride;
    const Json* re = body.find("reasoning_effort");
    if (re && re->isString()) effortOverride = re->asString();
    // R6 请求级覆盖
    int maxOverride = -1;
    const Json* mm = body.find("is_max_mode");
    if (mm && mm->isNumber()) maxOverride = (int)mm->asInt(0);
    if (!resolveSettings(*acc, model, effortOverride, maxOverride, caps, rs, perr)) {
        AccountPool::instance().release(acc, false, 0);
        sendJsonError(w, 400, "invalid_request_error", perr);
        return;
    }
    // 模型禁用检查
    const ModelConfig* mc = settings::get()->modelConfig(caps.configName);
    if (mc && mc->present && !mc->enabled) {
        AccountPool::instance().release(acc, false, 0);
        sendJsonError(w, 400, "invalid_request_error", "模型已禁用: " + caps.configName);
        return;
    }

    // 消息转换
    std::vector<ChatMessage> messages;
    const Json* arr = body.find("messages");
    if (!arr || !arr->isArray() || arr->size() == 0) {
        AccountPool::instance().release(acc, false, 0);
        sendJsonError(w, 400, "invalid_request_error", "messages 不能为空");
        return;
    }
    for (size_t i = 0; i < arr->size(); ++i) {
        const Json& m = arr->at(i);
        ChatMessage cm;
        cm.role = m.get("role", Json("user")).asString();
        const Json* c = m.find("content");
        if (c && c->isString()) cm.content = c->asString();
        else if (c && c->isArray()) {
            // 多模态：文本块聚合到 content，图片块进 parts（不得省略）
            std::vector<MessagePart> parts;
            bool hasImage = false;
            for (size_t k = 0; k < c->size(); ++k) {
                const Json& part = c->at(k);
                if (!part.isObject()) continue;
                std::string pt = part.get("type", Json("text")).asString();
                if (pt == "text" || pt == "input_text" || pt == "output_text") {
                    // 文本字段统一叫 "text"：input_text/output_text 是块类型名，
                    // 不是字段名——按类型名取字段会把文本静默丢成空消息。
                    std::string tx = part.get("text", Json("")).asString();
                    cm.content += tx;
                    MessagePart mp;
                    mp.type = MessagePart::Text;
                    mp.text = tx;
                    parts.push_back(mp);
                } else {
                    std::string url;
                    if (partImageUrl(part, url)) {
                        MessagePart mp;
                        mp.type = MessagePart::Image;
                        mp.url = url;
                        parts.push_back(mp);
                        hasImage = true;
                    }
                }
            }
            if (hasImage) cm.parts = std::move(parts);
        } else if (c && c->isNull()) cm.content = "";
        const Json* tcs = m.find("tool_calls");
        if (tcs && tcs->isArray()) {
            for (size_t k = 0; k < tcs->size(); ++k) {
                const Json& tc = tcs->at(k);
                ToolCall t;
                t.id = tc.get("id", Json("")).asString();
                const Json* fn = tc.find("function");
                if (fn) {
                    t.name = fn->get("name", Json("")).asString();
                    const Json* args = fn->find("arguments");
                    if (args && args->isString()) t.arguments = args->asString();
                    else if (args) t.arguments = args->dump();
                }
                // 规则 2：name 为空的条目剔除
                if (!t.name.empty()) cm.toolCalls.push_back(t);
            }
        }
        if (cm.role == "tool") {
            cm.toolCallId = m.get("tool_call_id", Json("")).asString();
            cm.toolName = m.get("name", Json("")).asString();
        }
        messages.push_back(cm);
    }
    // 工具
    std::vector<ToolDef> tools;
    normalizeOpenAiTools(body.get("tools", Json()), body.get("tool_choice", Json()), tools);

    // ===== 本地实现的请求参数（上游协议不收，由反代执行）=====
    // stop：对最终公开正文生效（工具参数、reasoning 不受影响），最多 4 条
    std::vector<std::string> stops;
    {
        const Json* sp = body.find("stop");
        auto add = [&](const std::string& s) {
            if (!s.empty() && s.size() <= 1024 && stops.size() < 4) stops.push_back(s);
        };
        if (sp && sp->isString()) add(sp->asString());
        else if (sp && sp->isArray())
            for (size_t i = 0; i < sp->size(); ++i)
                if (sp->at(i).isString()) add(sp->at(i).asString());
    }
    // stream_options.include_usage：独立 usage 帧（OpenAI 规范行为）
    bool includeUsage = false;
    const Json* so = body.find("stream_options");
    if (so && so->isObject()) includeUsage = so->get("include_usage", Json(false)).asBool();
    // parallel_tool_calls 按 OpenAI 语义只是给模型的提示，不丢弃任何调用：
    // 模型输出多少个 tool_calls 就转发多少个，并行/串行由客户端自行决定。

    // max_tokens 有意不解析不透传：上游 solo 通道忽略请求级该字段（2026-09-19 实测
    // completion_tokens 不受其约束，输出上限由上游模型档案在服务端控制）。

    std::string chunkId = "chatcmpl-" + crypto::genUuid();
    long long created = (long long)time(nullptr);
    std::string modelName = rs.displayName.empty() ? rs.configName : rs.displayName;
    bool clientWantsStream = stream;

    auto makeChunkHead = [&]() {
        Json head = Json::object();
        head.set("id", Json(chunkId));
        head.set("object", Json("chat.completion.chunk"));
        head.set("created", Json(created));
        head.set("model", Json(modelName));
        return head;
    };

    // 汇总（非流式/最终帧需要）
    std::string allText, allReason;
    ToolAccumulator finalTools;

    if (clientWantsStream) {
        if (!w.beginStream(200, "text/event-stream")) {
            AccountPool::instance().release(acc, true, 0);
            return;
        }
        // OpenAI 规范：流式响应首帧携带 role，严格按规范聚合的 SDK 依赖它标注消息归属。
        // 独立成帧保证空响应/纯工具调用轮也有 role，与官方先发 role chunk 的行为一致。
        Json roleHead = makeChunkHead();
        Json roleCh = Json::array();
        Json roleChoice = Json::object();
        roleChoice.set("index", Json(0));
        Json roleDelta = Json::object();
        roleDelta.set("role", Json("assistant"));
        roleDelta.set("content", Json(""));
        roleChoice.set("delta", roleDelta);
        roleChoice.set("finish_reason", Json(nullptr));
        roleCh.push_back(roleChoice);
        roleHead.set("choices", roleCh);
        if (includeUsage) roleHead.set("usage", Json(nullptr));
        w.writeSse("", roleHead.dump());
    }

    // stop 本地过滤器
    StopFilter stopF;
    stopF.setStops(&stops);

    auto emitTextDelta = [&](const std::string& t) -> bool {
        if (!clientWantsStream) return true;
        if (t.empty()) return true;
        Json head = makeChunkHead();
        Json ch = Json::array();
        Json choice = Json::object();
        choice.set("index", Json(0));
        Json delta = Json::object();
        delta.set("content", Json(t));
        choice.set("delta", delta);
        choice.set("finish_reason", Json(nullptr));
        ch.push_back(choice);
        head.set("choices", ch);
        if (includeUsage) head.set("usage", Json(nullptr)); // 规范：include_usage 时常规帧 usage=null
        return w.writeSse("", head.dump());
    };

    // 使用记录计时：从发起上游请求起算（含工具循环）
    const auto usageStart = std::chrono::steady_clock::now();
    UpResult r = runChatPipeline(
        *acc, caps, rs, messages, tools,
        [&](const UpEvent& e) -> bool {
            // 记录
            if (e.type == UpEvent::Text) {
                std::string vis = stopF.feed(e.text);
                if (vis.empty()) return true; // stop 命中或尾巴扣住：本帧无可放出文本
                allText += vis;
                return emitTextDelta(vis);
            }
            if (e.type == UpEvent::Reason) {
                allReason += e.text;
                if (!clientWantsStream) return true;
                Json head = makeChunkHead();
                Json ch = Json::array();
                Json choice = Json::object();
                choice.set("index", Json(0));
                Json delta = Json::object();
                delta.set("reasoning_content", Json(e.text));
                choice.set("delta", delta);
                choice.set("finish_reason", Json(nullptr));
                ch.push_back(choice);
                head.set("choices", ch);
                if (includeUsage) head.set("usage", Json(nullptr));
                return w.writeSse("", head.dump());
            }
            if (e.type == UpEvent::ToolCall) {
                // 先放出被扣住的正文尾巴，保持"正文先于其后的工具调用"顺序
                std::string tail = stopF.flush();
                if (!tail.empty()) {
                    allText += tail;
                    if (!emitTextDelta(tail)) return false;
                }
                // 上游 done 时按完整调用下发事件（一次一个完整调用）
                finalTools.onEvent(e);
                if (!clientWantsStream) return true;
                Json head = makeChunkHead();
                Json ch = Json::array();
                Json choice = Json::object();
                choice.set("index", Json(0));
                Json delta = Json::object();
                Json tcs = Json::array();
                Json tc = Json::object();
                tc.set("index", Json(e.toolIndex >= 0 ? e.toolIndex : 0));
                if (!e.toolId.empty()) tc.set("id", Json(e.toolId));
                tc.set("type", Json("function"));
                Json fn = Json::object();
                if (!e.toolName.empty()) fn.set("name", Json(e.toolName));
                fn.set("arguments", Json(e.toolArgs));
                tc.set("function", fn);
                tcs.push_back(tc);
                delta.set("tool_calls", tcs);
                choice.set("delta", delta);
                choice.set("finish_reason", Json(nullptr));
                ch.push_back(choice);
                head.set("choices", ch);
                if (includeUsage) head.set("usage", Json(nullptr));
                return w.writeSse("", head.dump());
            }
            return true;
        });

    // stop 过滤器残余冲刷：未成命中的尾巴按普通正文放出。与提取器残余同门控，
    // 失败轮不冲刷（报错前不漏半句）。
    if (r.ok || r.clientAborted) {
        std::string tail = stopF.flush();
        if (!tail.empty()) {
            allText += tail;
            if (!emitTextDelta(tail)) r.clientAborted = true;
        }
    }

    auto finishRequest = [&](bool ok) {
        // 使用记录登记：token 来自上游 token_usage，积分差值由积分回填时落账
        if (ok) {
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
                AccountPool::instance().usageRecordPending(acc, caps, "/v1/chat/completions", inT, outT, cacheT, ms);
            }
        }
        AccountPool::instance().release(acc, ok, ok ? 0 : r.code);
        // 消耗不随聊天流下发（2026-09-19 探针实测），请求后异步补查积分近实时更新
        if (ok) AccountPool::instance().refreshCreditsAsync(acc);
        if (clientWantsStream) {
            if (!w.clientGone()) {
                // 最终帧
                Json head = makeChunkHead();
                Json ch = Json::array();
                Json choice = Json::object();
                choice.set("index", Json(0));
                choice.set("delta", Json::object());
                // 上游原生工具轮也可能报 stop；保留截断/过滤原因，其余工具轮报 tool_calls。
                std::string fr = r.finishReason.empty() ? "stop" : r.finishReason;
                if (stopF.hit()) fr = "stop";
                else if (fr != "length" && fr != "content_filter" && !finalTools.empty()) fr = "tool_calls";
                choice.set("finish_reason", Json(fr));
                ch.push_back(choice);
                head.set("choices", ch);
                if (includeUsage) {
                    // OpenAI 规范：include_usage 时 finish 帧 usage=null，
                    // 其后追加一帧空 choices + 整体 usage
                    head.set("usage", Json(nullptr));
                    w.writeSse("", head.dump());
                    Json uhead = makeChunkHead();
                    uhead.set("choices", Json::array());
                    uhead.set("usage", usageJson(r.usage.isObject() ? r.usage : Json::object()));
                    w.writeSse("", uhead.dump());
                } else {
                    if (r.usage.isObject() && r.usage.size() > 0) head.set("usage", usageJson(r.usage));
                    w.writeSse("", head.dump());
                }
                w.writeSse("", "[DONE]");
            }
            w.endStream();
        }
    };

    if (!r.ok && !r.clientAborted) {
        // 上游失败
        int httpErr = 502;
        std::string type = "api_error";
        upstreamErrorType(r, httpErr, type);
        std::string msg = r.error.empty() ? "上游请求失败" : r.error;
        if (clientWantsStream) {
            if (!w.clientGone()) {
                // SSE 已以 200 开始，无法再改状态行：发规范 error 事件后结束，
                // 绝不伪造 content + finish_reason=stop（那会让客户端把失败当成功）。
                Json errFrame = Json::object();
                Json err = Json::object();
                err.set("message", Json(msg));
                err.set("type", Json(type));
                if (r.code != 0) err.set("code", Json(std::to_string(r.code)));
                errFrame.set("error", err);
                w.writeSse("error", errFrame.dump());
                w.writeSse("", "[DONE]");
            }
            w.endStream();
            AccountPool::instance().release(acc, false, r.code);
        } else {
            finishRequest(false);
            sendJsonError(w, httpErr, type, msg);
        }
        return;
    }
    finishRequest(true);
    if (clientWantsStream) return;

    // 非流式响应
    Json resp = Json::object();
    resp.set("id", Json(chunkId));
    resp.set("object", Json("chat.completion"));
    resp.set("created", Json(created));
    resp.set("model", Json(modelName));
    Json ch = Json::array();
    Json choice = Json::object();
    choice.set("index", Json(0));
    Json msg = Json::object();
    msg.set("role", Json("assistant"));
    auto tacs = finalTools.take();
    if (!tacs.empty()) {
        // OpenAI 语义：正文与 tool_calls 可并存（模型先说明再调用），无文本才置 null
        msg.set("content", allText.empty() ? Json(nullptr) : Json(allText));
        Json tcs = Json::array();
        for (auto& t : tacs) {
            Json tc = Json::object();
            tc.set("id", Json(t.id));
            tc.set("type", Json("function"));
            Json fn = Json::object();
            fn.set("name", Json(t.name));
            fn.set("arguments", Json(t.args.empty() ? "{}" : t.args));
            tc.set("function", fn);
            tcs.push_back(tc);
        }
        msg.set("tool_calls", tcs);
    } else {
        msg.set("content", Json(allText));
    }
    if (!allReason.empty()) msg.set("reasoning_content", Json(allReason));
    choice.set("message", msg);
    std::string finishReason = r.finishReason.empty() ? "stop" : r.finishReason;
    if (stopF.hit()) finishReason = "stop";
    else if (finishReason != "length" && finishReason != "content_filter" && !tacs.empty()) finishReason = "tool_calls";
    choice.set("finish_reason", Json(finishReason));
    ch.push_back(choice);
    resp.set("choices", ch);
    resp.set("usage", usageJson(r.usage.isObject() ? r.usage : Json::object()));
    w.respond(200, "application/json", resp.dump());
}

// ---------- /v1/models / /v1/status / /health ----------

static void handleModels(const HttpRequest& req, HttpResponseWriter& w) {
    auto acc = AccountPool::instance().acquire(8000);
    if (!acc) {
        bool usable = AccountPool::instance().hasUsableAccount();
        sendJsonError(w, 503, "api_error",
                      usable ? "账号并发繁忙，请稍后重试" : "无可用账号");
        return;
    }
    std::string err;
    auto all = ModelCatalog::instance().all(*acc, err);
    AccountPool::instance().release(acc, true, 0);
    auto cfg = settings::get();
    Json data = Json::array();
    long long now = (long long)time(nullptr);
    for (auto& c : all) {
        const ModelConfig* mc = cfg->modelConfig(c.configName);
        if (mc && mc->present && !mc->enabled) continue;
        Json m = Json::object();
        m.set("id", Json(c.configName));
        m.set("object", Json("model"));
        m.set("created", Json(now));
        m.set("owned_by", Json("trae"));
        m.set("display_name", Json((mc && !mc->displayName.empty()) ? mc->displayName : c.displayName));
        m.set("max_mode", Json(c.maxMode));
        Json cws = Json::object();
        cws.set("default", Json(c.cwDefault));
        Json mx = Json::array();
        for (auto v : c.cwMax) mx.push_back(Json(v));
        cws.set("max", mx);
        m.set("context_window_size", cws);
        Json eopts = Json::array();
        for (auto& o : c.effortOptions) eopts.push_back(Json(o));
        for (auto& o : c.effortOptionsExt) eopts.push_back(Json(o));
        m.set("reasoning_effort_options", eopts);
        m.set("multimodal", Json(c.vision));
        data.push_back(m);
    }
    Json out = Json::object();
    out.set("object", Json("list"));
    out.set("data", data);
    if (!err.empty()) out.set("warning", Json(err));
    w.respond(200, "application/json", out.dump());
}

static void handleStatus(const HttpRequest& req, HttpResponseWriter& w) {
    auto cfg = settings::get();
    auto& pool = AccountPool::instance();
    Json out = Json::object();
    Json svc = Json::object();
    svc.set("host", Json(cfg->serviceHost));
    svc.set("port", Json(cfg->servicePort));
    svc.set("allowLan", Json(cfg->allowLan));
    out.set("service", svc);
    Json st = Json::object();
    st.set("reasoningEffort", Json(cfg->defaultReasoningEffort.empty() ? Json() : Json(cfg->defaultReasoningEffort)));
    st.set("isMaxMode", Json(cfg->defaultIsMaxMode));
    out.set("settings", st);
    Json accs = Json::array();
    for (auto& a : pool.accounts()) {
        Json o = Json::object();
        o.set("nickname", Json(a->nickname));
        o.set("edition", Json(a->editionId));
        o.set("credits", Json(a->credits.load()));
        o.set("active", Json(a->active.load()));
        o.set("lastUsed", Json(a->lastUsedTs.load()));
        accs.push_back(o);
    }
    out.set("accounts", accs);
    w.respond(200, "application/json", out.dump());
}

// ---------- 注册 ----------

void apiRegisterRoutes(HttpServer& server) {
    server.handler = [](const HttpRequest& req, HttpResponseWriter& w) {
        LOG_D("请求进入: %s %s", req.method.c_str(), req.path.c_str());
        const std::string& p = req.path;
        if (p == "/health") {
            Json o = Json::object();
            o.set("status", Json("ok"));
            o.set("time", Json((long long)time(nullptr)));
            w.respond(200, "application/json", o.dump());
            return;
        }
        // 浏览器预检不携带 API Key；实际请求仍走下面的鉴权。
        if (req.method == "OPTIONS" &&
            (p == "/v1/models" || p == "/v1/status" || p == "/v1/chat/completions" || p == "/v1/responses")) {
            std::string allowHeaders = "Authorization, Content-Type, X-API-Key";
            if (const auto* requested = req.header("access-control-request-headers")) {
                if (requested->size() > 8192 || requested->find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'*+-.^_`|~, \t") != std::string::npos) {
                    sendJsonError(w, 400, "invalid_request_error", "无效的跨域预检请求头");
                    return;
                }
                allowHeaders = *requested;
            }
            w.respond(200, "text/plain", "", {
                {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
                {"Access-Control-Allow-Headers", allowHeaders},
                {"Access-Control-Max-Age", "600"},
                {"Vary", "Access-Control-Request-Headers"}
            });
            return;
        }
        if (!checkAuth(req)) {
            sendJsonError(w, 401, "authentication_error", "API Key 无效", "invalid_api_key");
            return;
        }
        if (req.method == "GET" && p == "/v1/models") { handleModels(req, w); return; }
        if (req.method == "GET" && p == "/v1/status") { handleStatus(req, w); return; }
        if (req.method == "POST" && p == "/v1/chat/completions") { handleChatCompletions(req, w); return; }
        if (req.method == "POST" && p == "/v1/responses") {
            extern void responsesHandle(const HttpRequest&, HttpResponseWriter&);
            responsesHandle(req, w);
            return;
        }
        sendJsonError(w, 404, "invalid_request_error", "未知路径: " + p);
    };
}
