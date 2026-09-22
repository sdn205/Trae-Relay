// Upstream.h - 上游 solo 通道：归一化请求/事件流、请求头构造、能力 clamp
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/Http.h"
#include "common/Json.h"

class AccountPool;
struct Account;

// ---- UTF-8 边界工具 ----
// 把字节长度 n 回退到最近的 UTF-8 字符边界，返回 [0, n] 内合法的最大切点。
// 所有按字节长度切分文本的地方都必须用它：切点多字节字符中间会让单独一帧
// （SSE data / 其中的 JSON 字符串）不是合法 UTF-8，端上按帧解码即显示为 U+FFFD。
inline size_t utf8SafeCut(const std::string& s, size_t n) {
    if (n >= s.size()) return s.size();
    size_t i = n;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i; // 回退到字符首字节
    if (i == 0) return 0;
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t need = c < 0x80 ? 1
                : (c & 0xE0) == 0xC0 ? 2
                : (c & 0xF0) == 0xE0 ? 3
                : (c & 0xF8) == 0xF0 ? 4 : 1;
    return (i + need <= n) ? n : i;
}

struct ToolCall {
    std::string id, name, arguments;
};

// 工具声明（parameters 为 JSON schema 字符串）
struct ToolDef {
    std::string name, description, parameters;
};

// 多模态内容块（文本/图片）。图片 url 为 http(s):// 链接或 data:<mime>;base64,...
struct MessagePart {
    enum Type { Text, Image } type = Text;
    std::string text;                    // Text：文本
    std::string url;                     // Image：图片 URL / data URI
};

// 归一化对话消息（OpenAI 风格）
struct ChatMessage {
    std::string role;                    // system / user / assistant / tool
    std::string content;                 // 纯文本聚合（parts 为空时是权威内容）
    std::vector<MessagePart> parts;      // 多模态内容（非空时序列化优先，图片不得省略）
    std::vector<ToolCall> toolCalls;     // assistant 历史调用
    std::string toolCallId;              // tool 角色的对应 id
    std::string toolName;                // tool 角色的工具名
};

// 从多模态 content part 提取图片 URL：OpenAI image_url、Responses input_image，
// 以及通用的 image/source 块（url 直链或 base64 data）。
inline bool partImageUrl(const Json& part, std::string& url) {
    if (!part.isObject()) return false;
    auto fromSource = [&](const Json& src) -> bool {
        std::string st = src.get("type", Json("")).asString();
        const Json* u = src.find("url");
        if ((st == "url" || st.empty()) && u && u->isString() && !u->asString().empty()) {
            url = u->asString();
            return true;
        }
        const Json* d = src.find("data");
        if ((st == "base64" || st.empty()) && d && d->isString() && !d->asString().empty()) {
            const Json* mt = src.find("media_type");
            std::string mime = mt && mt->isString() && !mt->asString().empty() ? mt->asString() : "image/png";
            url = "data:" + mime + ";base64," + d->asString();
            return true;
        }
        return false;
    };
    const Json* iu = part.find("image_url");
    if (iu) {
        if (iu->isString() && !iu->asString().empty()) { url = iu->asString(); return true; }
        if (iu->isObject()) {
            const Json* u = iu->find("url");
            if (u && u->isString() && !u->asString().empty()) { url = u->asString(); return true; }
        }
    }
    const Json* im = part.find("image");
    if (im) {
        if (im->isString() && !im->asString().empty()) { url = im->asString(); return true; }
        if (im->isObject()) {
            const Json* u = im->find("url");
            if (u && u->isString() && !u->asString().empty()) { url = u->asString(); return true; }
            const Json* src = im->find("source");
            if (src && src->isObject() && fromSource(*src)) return true;
        }
    }
    const Json* src = part.find("source");
    if (src && src->isObject() && fromSource(*src)) return true;
    // Responses input_image：{image_url:"..."} 或 {data,media_type}
    const Json* d = part.find("data");
    if (d && d->isString() && !d->asString().empty()) {
        const Json* mt = part.find("media_type");
        std::string mime = mt && mt->isString() && !mt->asString().empty() ? mt->asString() : "image/png";
        url = "data:" + mime + ";base64," + d->asString();
        return true;
    }
    const Json* u = part.find("url");
    if (u && u->isString() && !u->asString().empty()) {
        const std::string& v = u->asString();
        if (v.rfind("http://", 0) == 0 || v.rfind("https://", 0) == 0 || v.rfind("data:", 0) == 0) {
            url = v;
            return true;
        }
    }
    return false;
}

// 模型能力（由 ModelCatalog 解析，档位/Max 判定依据）
struct ModelCaps {
    std::string configName, modelName, displayName;
    int configSource = 1;
    std::string provider;
    bool isPreset = true;
    bool maxMode = false;                       // 模型是否支持 Max（更大上下文）
    long long cwDefault = 200000;
    std::vector<long long> cwMax;               // context_window_size.max 数组
    long long promptMaxTokens = 936000;
    long long maxTokens = 64000;
    int maxTurn = 500;
    bool supportThinking = false;
    std::vector<std::string> effortOptions;     // 内置: reasoning_effort_config.options
    std::vector<std::string> effortOptionsExt;  // 外部: reasoning_effort_options
    std::string effortDefault;                  // default_level
    bool present = false;                       // 是否在模型表中找到
    bool vision = false;                        // 支持图片输入（多模态）
    bool ideChatCapable = false;                // IDE 详情目录可用
    std::string ideFunction;                    // chat_v3 / builder_v3
    std::string maxModelName;                   // get_detail_param 中 __max 档案的 model_name（Max 模式使用）
    // 计费倍率（来自 display_contact_config，0 = 上游未给）：
    double rateBase = 0;                        // consumption_rate.data.rate 基础倍率
    double rateMember = 0;                      // discount.data.consumption_rate 会员倍率（会员全天价）
    int memberDiscountOff = 0;                  // discount.data.member_discount 折扣百分比（50 = 5折）
    double rateActivity = 0;                    // activity_discount.data.current.consumption_rate 活动倍率
    double rateActivityBefore = 0;              // activity_discount.data.current.before_consumption_rate 活动前原价
    double rateActivityMember = 0;              // activity_discount.data.member.after_consumption_rate 活动会员全天价
    std::string activityType;                   // activity_discount.data.current.discount_type（off_peak / ...）
    // 闲时折扣时间窗（off_peak.time_windows）：北京时间，minute-of-day，end 不含 1440=24:00
    struct OffPeakWindow {
        std::vector<int> weekdays;              // ISO 星期 1=周一 … 7=周日
        int startMinute = 0;
        int endMinute = 0;
    };
    std::vector<OffPeakWindow> offPeakWindows;
};

// 实时计费倍率视图：会员身份（ide_user_pay_status.user_pay_identity）+ 北京时间闲时窗口
// 解析出"此刻"的实际倍率。官方显示规则（trae-chat-core i18n 实证）：
//  - 会员（identity != 0）：全天享会员价（discount.data.consumption_rate）；
//    闲时类模型会员全天享 member.after_consumption_rate（"会员专享x折"）。
//  - 非会员：闲时窗口内享 off_peak 活动价，窗口外原价（"非会员用户仅空闲时段x折"）。
struct RateView {
    double effective = 0;      // 此刻实际倍率（0 = 目录未给，UI 隐藏）
    double original = 0;       // 原价（rateBase）
    bool member = false;       // 账号是付费身份
    bool inOffPeak = false;    // 此刻在北京时间闲时窗口内
    int memberOffPercent = 0;  // 会员折扣百分比（50 = 5折）
    bool memberTeaser = false; // 非会员但模型有会员价（升级引导）
    const char* activityTag = nullptr; // 非空表示当前生效价来自活动："闲时折扣"/"专属补贴"
};
// payIdentity：Account::payIdentity（-1 未知按非会员算，保守口径）
RateView modelRateView(const ModelCaps& c, int payIdentity);

// 每请求归一化参数（已经 Config×Caps 取交集 clamp，见 ResolveSettings）
struct UpRequest {
    std::string model;                    // 解析后的 config_name
    std::vector<ChatMessage> messages;
    std::vector<ToolDef> tools;           // 归一化工具声明
    bool stream = true;
    std::string reasoningEffort;          // "" = 不生效（solo 走 system 前缀注入）
    bool maxMode = false;                 // Max / 1M 上下文
    long long maxContextWindow = 0;       // Max 上下文窗口值
};

// 归一化事件
struct UpEvent {
    enum Type { Text, Reason, ToolCall, Finish, Error, Meta } type = Meta;
    std::string text;                     // Text/Reason 增量；Error=消息
    int code = 0;                         // Error 上游码
    int toolIndex = -1;
    std::string toolId, toolName, toolArgs;
    std::string finishReason;
    std::string model;                    // 实际模型
};
using UpSink = std::function<bool(const UpEvent&)>;   // false = 客户端断开，需中止

struct UpResult {
    bool ok = false;
    bool clientAborted = false;
    std::string error;
    int code = 0;                         // 上游业务码
    int httpStatus = 0;
    std::string finishReason;
    Json usage;
    std::string model;                    // 实际模型名
};

// 请求头构造（IDE 头组）
http::Headers ideHeaders(const Account& acc, const std::string& ideVersion,
                         const std::string& ideVersionCode, bool sse, const char* appId = nullptr);

// solo 通道实现（UpstreamSolo.cpp）
UpResult upstreamSolo(const UpRequest& req, Account& acc, const ModelCaps& caps, const UpSink& sink);

// 固定走 solo 通道；401/1001 时刷新令牌重试一次
UpResult upstreamDispatch(const UpRequest& req, Account& acc, const ModelCaps& caps, const UpSink& sink);
void logRequestFailure(const char* endpoint, const std::string& model, const Account& acc, const UpResult& result);

// 档位白名单校验 + clamp（不合法 → 不生效，返回 ""）
std::string clampEffort(const ModelCaps& caps, const std::string& want);
// 线上档位别名归一（extra_high/max → xhigh，light → low）
std::string normalizeEffortValue(std::string v);
// Max 能力 clamp
bool clampMaxMode(const ModelCaps& caps, bool want, long long cfgWindow, long long& outWindow);

// 共享工具：累计快照 → 增量
// allowReplace=true（正文）：非前缀或变短 → 整体替换；false（思考快照）：变短 → 丢弃帧
bool snapshotDelta(std::string& last, const std::string& snap, std::string& delta, bool allowReplace = true);
