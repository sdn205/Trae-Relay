// UpstreamSolo.cpp - 通道 C：/api/agent/v3/llm_utils_chat（solo_work_lite 纯文本）
#include "upstream/Upstream.h"
#include "accounts/AccountPool.h"
#include "common/Crypto.h"
#include "common/Log.h"
#include "common/Settings.h"
#include "common/SseParser.h"
#include <map>

static const char* kSoloHost = "https://trae-api-cn.mchost.guru";

// 历史/客户端文本中若残留 opencode 原生标签，上游会将其识别为自己的工具
// 协议并在输出中途挂起。统一改写为中性标签后再上行。
static void sanitizeToolTags(std::string& text) {
    static const char* kFrom[] = {
        "</opencode_tool_call>", "<opencode_tool_call>",
        "</opencode_tool_result>", "<opencode_tool_result"
    };
    static const char* kTo[] = {
        "</tool_invoke>", "<tool_invoke>",
        "</tool_result>", "<tool_result"
    };
    for (size_t i = 0; i < 4; ++i) {
        size_t pos = 0;
        while ((pos = text.find(kFrom[i], pos)) != std::string::npos) {
            text.replace(pos, strlen(kFrom[i]), kTo[i]);
            pos += strlen(kTo[i]);
        }
    }
}

// 构造一条消息的 content parts：文本统一过标签消毒，图片原样透传（不得省略）。
static Json soloContent(const ChatMessage& m) {
    Json arr = Json::array();
    auto pushText = [&](std::string text) {
        sanitizeToolTags(text);
        if (text.empty()) return;
        Json t = Json::object();
        t.set("type", Json("text"));
        t.set("text", Json(text));
        arr.push_back(t);
    };
    if (!m.parts.empty()) {
        for (const auto& p : m.parts) {
            if (p.type == MessagePart::Text) {
                pushText(p.text);
            } else if (!p.url.empty()) {
                Json im = Json::object();
                im.set("type", Json("image_url"));
                Json iu = Json::object();
                iu.set("url", Json(p.url));
                im.set("image_url", iu);
                arr.push_back(im);
            }
        }
    } else {
        pushText(m.content);
    }
    return arr;
}

// 追加一条纯文本消息（system 协议注入等）；空文本不追加
static void appendSoloText(Json& messages, const std::string& role, std::string text) {
    sanitizeToolTags(text);
    if (text.empty()) return;
    Json m = Json::object();
    m.set("role", Json(role));
    Json arr = Json::array();
    Json t = Json::object();
    t.set("type", Json("text"));
    t.set("text", Json(text));
    arr.push_back(t);
    m.set("content", arr);
    messages.push_back(m);
}

// 追加一条归一化消息（含多模态 parts；含图片时即使无文本也必须追加）
static void appendSoloChatMessage(Json& messages, const std::string& role, const ChatMessage& m) {
    Json content = soloContent(m);
    if (content.size() == 0) return;
    Json msg = Json::object();
    msg.set("role", Json(role));
    msg.set("content", content);
    messages.push_back(msg);
}

UpResult upstreamSolo(const UpRequest& req, Account& acc, const ModelCaps& caps, const UpSink& sink) {
    UpResult r;
    auto cfg = settings::get();
    std::string configName = caps.configName.empty() ? req.model : caps.configName;

    Json body = Json::object();
    Json messages = Json::array();
    // 工具调用走上游原生 function calling（body.tools），不再注入文本协议
    for (const auto& m : req.messages) {
        std::string role = m.role == "developer" ? "system" : m.role;
        if (role == "tool") {
            // 原生 function calling 结果：role=tool + tool_call_id 关联；
            // 文本同样过标签消毒（工具输出里可能包含协议标签源码）。
            Json msg = Json::object();
            msg.set("role", Json("tool"));
            msg.set("tool_call_id", Json(m.toolCallId.empty() ? "unknown" : m.toolCallId));
            if (!m.toolName.empty()) msg.set("name", Json(m.toolName));
            msg.set("content", soloContent(m));
            messages.push_back(msg);
            continue;
        }
        if (role == "assistant" && !m.toolCalls.empty()) {
            // 原生 function calling 历史：tool_calls 数组，函数体键名为
            // 上游要求的 function_call；name 为空的条目剔除（必填校验）。
            Json msg = Json::object();
            msg.set("role", Json("assistant"));
            msg.set("content", soloContent(m));
            Json tcs = Json::array();
            for (const auto& tc : m.toolCalls) {
                if (tc.name.empty()) continue;
                Json fc = Json::object();
                fc.set("name", Json(tc.name));
                fc.set("arguments", Json(tc.arguments.empty() ? "{}" : tc.arguments));
                Json t = Json::object();
                t.set("id", Json(tc.id.empty() ? ("call_" + crypto::genUuid()) : tc.id));
                t.set("type", Json("function"));
                t.set("function_call", fc);
                tcs.push_back(t);
            }
            msg.set("tool_calls", tcs);
            messages.push_back(msg);
            continue;
        }
        if (role == "system") {
            appendSoloChatMessage(messages, "system", m);
            continue;
        }
        appendSoloChatMessage(messages, role, m);
    }
    if (messages.empty()) appendSoloText(messages, "user", "");
    body.set("messages", messages);
    // 模型目录只保留 get_detail_param 的 IDE 主目录档案，function 固定
    // chat_v3 / solo_agent，不再有 solo_work_lite / builder_v3 回退。
    bool ideChat = caps.ideChatCapable;
    std::string ideFunction = caps.ideFunction.empty() ? "chat_v3" : caps.ideFunction;
    body.set("function", Json(ideChat ? ideFunction : "chat_v3"));
    body.set("stream", Json(true));
    body.set("config_name", Json(configName));
    // Max / 1M 上下文：切换到目录中的 __max 档案模型名，并声明大上下文窗口。
    std::string upstreamModel = caps.modelName;
    if (req.maxMode) {
        if (!caps.maxModelName.empty()) upstreamModel = caps.maxModelName;
        body.set("mode_type", Json(1));
        long long cw = req.maxContextWindow > 0 ? req.maxContextWindow : 1000000;
        body.set("context_window_size", Json(cw));
        LOG_I("Max 模式：config=%s model=%s context_window_size=%lld",
              configName.c_str(), upstreamModel.c_str(), cw);
    }
    body.set("model", Json(ideChat && !upstreamModel.empty() ? upstreamModel : configName));
    body.set("model_name", Json(ideChat && !upstreamModel.empty() ? upstreamModel : configName));
    body.set("config_source", Json(caps.configSource));
    body.set("is_custom_model", Json(false));
    body.set("provider", Json(caps.provider));
    // Trae 原生思考档位：官方请求体使用顶层 reasoning_effort 字段。
    if (!req.reasoningEffort.empty()) {
        body.set("reasoning_effort", Json(req.reasoningEffort));
        LOG_I("原生思考档位：effort=%s source=reasoning_effort", req.reasoningEffort.c_str());
    }
    // 原生 function calling：tools[].function.parameters 上游要求为 JSON
    // 字符串（OpenAI 为对象）；工具调用以 tool_calls 增量事件返回。
    if (!req.tools.empty()) {
        Json toolsArr = Json::array();
        for (const auto& td : req.tools) {
            Json fn = Json::object();
            fn.set("name", Json(td.name));
            fn.set("description", Json(td.description));
            fn.set("parameters", Json(td.parameters.empty() ? "{}" : td.parameters));
            Json wrap = Json::object();
            wrap.set("type", Json("function"));
            wrap.set("function", fn);
            toolsArr.push_back(wrap);
        }
        body.set("tools", toolsArr);
        body.set("tool_choice", Json("auto"));
    }

    http::Headers hd = ideHeaders(acc, cfg->ideVersion, cfg->ideVersionCode, true);
    auto stream = http::openStream("POST", std::string(kSoloHost) + "/api/agent/v3/llm_utils_chat",
                                   hd, body.dump(), cfg->firstEventTimeoutSec * 1000,
                                   cfg->upstreamTimeoutSec * 1000);
    if (!stream || stream->status == 0) {
        r.error = "打开失败: " + (stream ? stream->error : std::string("null"));
        return r;
    }
    r.httpStatus = stream->status;
    if (stream->status != 200) {
        std::string line, errBody;
        while (stream->readLine(line) && errBody.size() < 800) errBody += line + "\n";
        r.httpStatus = stream->status;
        r.code = stream->status == 401 ? 1001 : 0;
        r.error = "solo chat HTTP " + std::to_string(stream->status) + ": " + errBody.substr(0, 400);
        return r;
    }

    bool sawDone = false, aborted = false;
    // Each stream owns its queue count, including on errors and early returns.
    struct QueueScope {
        Account& account;
        bool waiting = false;
        void set(bool value) {
            if (waiting == value) return;
            waiting = value;
            if (value) ++account.queued;
            else --account.queued;
        }
        ~QueueScope() { set(false); }
    } queue{acc};
    std::string lastResp, lastReason;
    // 原生 function calling：上游 tool_calls 为流式增量（首帧 id/name，
    // 后续帧按 index 追加 arguments 片段）。先按 index 累积，done 时一次性
    // 下发完整调用，与下游"完整工具事件"语义对齐。
    struct NativeTool { std::string id, name, args; };
    std::map<int, NativeTool> nativeTools;
    auto emit = [&](UpEvent e) -> bool {
        if (!sink(e)) {
            aborted = true;
            return false;
        }
        return true;
    };
    auto flushNativeTools = [&]() -> bool {
        for (auto& kv : nativeTools) {
            if (kv.second.name.empty()) continue;
            UpEvent e;
            e.type = UpEvent::ToolCall;
            e.toolIndex = kv.first;
            e.toolId = kv.second.id;
            e.toolName = kv.second.name;
            e.toolArgs = kv.second.args.empty() ? "{}" : kv.second.args;
            if (!emit(e)) return false;
        }
        return true;
    };
    SseParser sse;
    sse.setHandler([&](const std::string& evName, const std::string& payload) -> bool {
        std::string ev = evName.empty() ? "message" : evName;
        for (auto& c : ev) c = (char)tolower((unsigned char)c);
        if (ev == "request_wait_in_queue" || ev == "queue_begin") {
            queue.set(true);
            return true;
        }
        if (ev == "queue_end") {
            queue.set(false);
            return true;
        }
        if (ev == "done" || ev == "error" || payload == "[DONE]") queue.set(false);
        if (payload == "[DONE]") {
            sawDone = true;
            return false;
        }
        Json data;
        if (!Json::parse(payload, data)) return true;
        if (ev == "error") {
            r.code = (int)data.get("code", Json(0)).asInt(0);
            r.error = "trae " + std::to_string(r.code) + ": " + data.get("message", Json("")).asString();
            return false;
        }
        if (ev == "token_usage" || ev == "notify_usage" || ev == "usage") {
            // usage 提取:优先嵌套 usage 对象(协议预留);否则读平铺字段
            // (2026-09-19 探针实测:上游 token_usage 为平铺
            // prompt_tokens/completion_tokens/total_tokens,见 dist/sse_probe.log)
            const Json* u = data.find("usage");
            if (u && u->isObject()) {
                r.usage = *u;
            } else {
                long long pt = data.get("prompt_tokens", Json(0)).asInt(0);
                long long ct = data.get("completion_tokens", Json(0)).asInt(0);
                long long tt = data.get("total_tokens", Json(0)).asInt(0);
                if (tt == 0) tt = pt + ct;
                if (pt > 0 || ct > 0 || tt > 0) {
                    r.usage = Json::object();
                    r.usage.set("prompt_tokens", Json(pt));
                    r.usage.set("completion_tokens", Json(ct));
                    r.usage.set("total_tokens", Json(tt));
                    Json pd = Json::object();
                    pd.set("cached_tokens", Json(data.get("cache_read_input_tokens", Json(0)).asInt(0)));
                    pd.set("cache_creation_input_tokens", Json(data.get("cache_creation_input_tokens", Json(0)).asInt(0)));
                    r.usage.set("prompt_tokens_details", pd);
                    Json cd = Json::object();
                    cd.set("reasoning_tokens", Json(data.get("reasoning_tokens", Json(0)).asInt(0)));
                    r.usage.set("completion_tokens_details", cd);
                }
            }
            AccountPool::instance().updateCreditsFromEvent(acc, data);
            return true;
        }
        if (ev == "progress_notice" || ev == "metadata" || ev == "extra_info" || ev == "timing_cost")
            return true;
        if (ev == "done") {
            // 工具调用参数在 done 前已累积完整，先于结束事件下发
            if (!flushNativeTools()) { aborted = true; return false; }
            std::string fr = data.get("finish_reason", Json("")).asString();
            r.finishReason = fr.empty() ? "stop" : fr;
            sawDone = true;
            return false;
        }
        if (ev == "output" || ev == "message" || ev == "text") {
            queue.set(false);
            // 原生工具调用（流式增量，按 index 累积，done 时统一 flush）
            const Json* tcs = data.find("tool_calls");
            if (tcs && tcs->isArray()) {
                for (size_t i = 0; i < tcs->size(); ++i) {
                    const Json& tc = tcs->at(i);
                    int idx = (int)tc.get("index", Json((double)i)).asInt((long long)i);
                    std::string id = tc.get("id", Json("")).asString();
                    std::string name, argsField, partialField;
                    const Json* fc = tc.find("function_call");
                    if (fc && fc->isObject()) {
                        name = fc->get("name", Json("")).asString();
                        argsField = fc->get("arguments", Json("")).asString();
                        partialField = fc->get("partial_arguments", Json("")).asString();
                    } else {
                        // 兼容 OpenAI 形态 function.name / function.arguments
                        const Json* fn = tc.find("function");
                        if (fn && fn->isObject()) {
                            name = fn->get("name", Json("")).asString();
                            argsField = fn->get("arguments", Json("")).asString();
                            partialField = fn->get("partial_arguments", Json("")).asString();
                        }
                    }
                    NativeTool& nt = nativeTools[idx];
                    if (!id.empty()) nt.id = id;
                    if (!name.empty()) nt.name = name;
                    // 参数分片口径：partial_arguments 是本帧增量片段，直接追加；
                    // arguments 在不同模型上既可能是增量片段，也可能是从头开始的
                    // 累计快照——若新串以已累积内容为前缀则按快照覆盖，否则追加。
                    if (!partialField.empty()) {
                        nt.args += partialField;
                    } else if (!argsField.empty()) {
                        if (nt.args.empty()) {
                            nt.args = argsField;
                        } else if (argsField.size() >= nt.args.size() &&
                                   argsField.compare(0, nt.args.size(), nt.args) == 0) {
                            nt.args = argsField;  // 累计快照
                        } else if (argsField == nt.args) {
                            // 重复快照，忽略
                        } else {
                            nt.args += argsField; // 增量片段
                        }
                    }
                }
            }
            // 文本：新格式 content（增量） / 旧格式 response（累计快照）
            const Json* txt = data.find("content");
            if (txt && txt->isString() && !txt->asString().empty()) {
                UpEvent e;
                e.type = UpEvent::Text;
                e.text = txt->asString();
                if (!emit(e)) return false;
            }
            txt = data.find("response");
            if (txt && txt->isString() && !txt->asString().empty()) {
                std::string delta;
                if (snapshotDelta(lastResp, txt->asString(), delta)) {
                    UpEvent e;
                    e.type = UpEvent::Text;
                    e.text = delta;
                    if (!emit(e)) return false;
                }
            }
            // 思考：reasoning（增量） / reasoning_content（快照）
            const Json* rs = data.find("reasoning");
            if (rs && rs->isString() && !rs->asString().empty()) {
                UpEvent e;
                e.type = UpEvent::Reason;
                e.text = rs->asString();
                if (!emit(e)) return false;
            }
            rs = data.find("reasoning_content");
            if (rs && rs->isString() && !rs->asString().empty()) {
                std::string delta;
                if (snapshotDelta(lastReason, rs->asString(), delta)) {
                    UpEvent e;
                    e.type = UpEvent::Reason;
                    e.text = delta;
                    if (!emit(e)) return false;
                }
            }
        }
        return true;
    });

    std::string line;
    while (!sawDone && !aborted) {
        if (!stream->readLine(line)) break;
        sse.feed(line.c_str(), line.size());
        sse.feed("\n", 1);
    }
    sse.finish();
    if (aborted) {
        r.clientAborted = true;
        return r;
    }
    if (!r.error.empty()) return r;
    if (!sawDone) {
        r.error = (queue.waiting ? "上游排队期间：" : "") +
            (stream->error.empty() ? std::string("上游提前关闭流，未收到结束事件") : stream->error);
        r.code = -1;
        return r;
    }
    r.ok = true;
    r.model = configName;
    return r;
}
