// Facade.h - 本地 API 门面：路由、鉴权、OpenAI 兼容（chat completions / responses）、设置解析
#pragma once
#include "accounts/AccountPool.h"
#include "common/HttpServer.h"
#include "upstream/ModelCatalog.h"
#include "upstream/Upstream.h"

// 解析后的请求设置（用户意愿 × 模型能力）
struct ResolvedSettings {
    std::string requested;      // 客户端请求的模型名
    std::string configName;     // 解析后的 config_name
    std::string displayName;
    std::string effort;         // clamp 后的档位；"" = 不生效
    std::string effortWant;     // 配置意愿（日志）
    bool maxMode = false;       // clamp 后
    long long window = 0;       // Max 上下文窗口
};

// 设置解析：effortOverride 传 "" 表示无请求级覆盖；maxOverride 传 -1 表示无请求级覆盖
bool resolveSettings(Account& acc, const std::string& requested, const std::string& effortOverride,
                     int maxOverride, ModelCaps& caps, ResolvedSettings& rs, std::string& err);

// 工具累积器（raw 通道增量 / 其他通道全量）
struct ToolAcc {
    int index = 0;
    std::string id, name, args;
};
class ToolAccumulator {
public:
    void onEvent(const UpEvent& e);
    std::vector<ToolAcc> take() { return std::move(m_tools); }
    bool empty() const { return m_tools.empty(); }
private:
    std::vector<ToolAcc> m_tools;
    ToolAcc* find(int idx);
};

// 通用对话管线：单轮上游请求，仅转发上游原生工具事件。
// 工具调用（含 web_search 等任何函数）一律作为 ToolCall 事件外发，由客户端执行，
// 反代不拦截、不本地执行；模型回 tool_calls 即结束本轮。
UpResult runChatPipeline(Account& acc, ModelCaps& caps, const ResolvedSettings& rs,
                         std::vector<ChatMessage> messages, std::vector<ToolDef> tools,
                         const UpSink& sink);

// OpenAI 风格 tools 归一化（含 tool_choice 三规则改写）
void normalizeOpenAiTools(const Json& toolsArr, const Json& toolChoice, std::vector<ToolDef>& out);

// 上游错误码 → 下游 HTTP 状态与错误类型
void upstreamErrorType(const UpResult& r, int& httpStatus, std::string& type);

// 注册全部本地 API 路由到服务器
void apiRegisterRoutes(class HttpServer& server);

// /v1/responses 处理（ResponsesApi.cpp）
void responsesHandle(const HttpRequest& req, HttpResponseWriter& w);
