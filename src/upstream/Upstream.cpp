// Upstream.cpp - 公共部分：请求头构造 / clamp / 快照增量 / 通道调度
#include "upstream/Upstream.h"
#include "accounts/AccountPool.h"
#include "common/Crypto.h"
#include "common/Log.h"
#include <algorithm>
#include <ctime>
#include <map>

http::Headers ideHeaders(const Account& acc, const std::string& ideVersion,
                         const std::string& ideVersionCode, bool sse, const char* appId) {
    http::Headers h;
    h.set("Content-Type", "application/json");
    h.set("Authorization", "Cloud-IDE-JWT " + acc.auth.accessToken);
    h.set("X-Cloudide-Token", acc.auth.accessToken);
    h.set("X-Ide-Token", acc.auth.accessToken);
    h.set("X-Uid", acc.auth.userId);
    h.set("x-uid", acc.auth.userId);
    h.set("x-app-id", appId ? appId : "6eefa01c-1036-4c7e-9ca5-d891f63bfcd8");
    h.set("x-app-version", "default");
    // 官方客户端使用与当前 IDE 构建一致的版本码；旧的固定值会让模型接口返回旧目录。
    h.set("x-app-version-code", ideVersionCode);
    h.set("x-device-id", acc.deviceId.empty() ? (acc.machineId.empty() ? "0" : acc.machineId)
                                                   : acc.deviceId);
    h.set("x-machine-id", acc.machineId.empty() ? "0" : acc.machineId);
    h.set("x-request-id", crypto::genUuid());
    h.set("x-ide-version", ideVersion);
    h.set("x-ide-version-code", ideVersionCode);
    h.set("x-ide-version-type", "stable");
    h.set("x-device-cpu", "AMD");
    h.set("x-device-brand", "83DG");
    h.set("x-device-type", "windows");
    h.set("x-os-version", "Windows 11 Pro");
    h.set("x-system-type", "Windows");
    h.set("Accept", sse ? "text/event-stream" : "application/json");
    h.set("Connection", "keep-alive");
    return h;
}

// ---------- R5 / R6 clamp（用户意愿 × 模型能力） ----------
// Trae 当前模型目录原生档位为 low/high/extra_high。
std::string normalizeEffortValue(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)tolower(c); });
    return v;
}

std::string clampEffort(const ModelCaps& caps, const std::string& wantRaw) {
    if (wantRaw.empty()) {
        // 跟随模型 default_level
        if (!caps.effortDefault.empty() && caps.supportThinking)
            return normalizeEffortValue(caps.effortDefault);
        return "";
    }
    if (!caps.supportThinking) return ""; // 模型表没有思考配置 → 不生效
    std::string want = normalizeEffortValue(wantRaw);
    auto hasOption = [&](const std::string& target) {
        for (auto& o : caps.effortOptions)
            if (normalizeEffortValue(o) == target) return true;
        for (auto& o : caps.effortOptionsExt)
            if (normalizeEffortValue(o) == target) return true;
        return false;
    };
    if (!caps.effortOptions.empty() || !caps.effortOptionsExt.empty())
        return hasOption(want) ? want : ""; // 不在 options 中 → 绝不透传
    return "";
}

// ---------- 实时计费倍率（会员身份 × 闲时窗口） ----------
// 时间窗按北京时间（UTC+8 固定偏移，无夏令时），与官方口径一致。
static void beijingNow(int& isoWeekday, int& minuteOfDay) {
    time_t now = time(nullptr) + 8 * 3600;
    struct tm utc {};
    gmtime_s(&utc, &now);
    isoWeekday = utc.tm_wday == 0 ? 7 : utc.tm_wday;
    minuteOfDay = utc.tm_hour * 60 + utc.tm_min;
}

RateView modelRateView(const ModelCaps& c, int payIdentity) {
    RateView v;
    // 活动模型的目录 rate 字段是折后现价（limited/subsidy/off_peak 三类实测均如此），
    // 原价必须取 current.before_consumption_rate；无活动才用 rateBase 当原价。
    v.original = c.rateActivityBefore > 0 ? c.rateActivityBefore : c.rateBase;
    v.member = payIdentity > 0; // 0=Free；-1 未知按非会员（保守）
    v.memberOffPercent = c.memberDiscountOff;
    v.effective = v.original;
    // 活动价（activity_discount）
    if (c.rateActivity > 0) {
        bool offPeakKind = !c.offPeakWindows.empty() || c.activityType == "off_peak";
        const char* tag = c.activityType == "limited"  ? "限时折扣"
                          : c.activityType == "subsidy" ? "专属补贴"
                          : offPeakKind                ? "闲时折扣"
                                                         : "专属补贴";
        double candidate = 0;
        if (offPeakKind) {
            int wd = 0, mod = 0;
            beijingNow(wd, mod);
            for (auto& w : c.offPeakWindows) {
                bool dayOk = w.weekdays.empty();
                for (int d : w.weekdays)
                    if (d == wd) dayOk = true;
                if (dayOk && mod >= w.startMinute && mod < w.endMinute) {
                    v.inOffPeak = true;
                    break;
                }
            }
            if (v.member) {
                // 会员全天享活动会员价（官方文案"会员专享x折"）
                candidate = c.rateActivityMember > 0 ? c.rateActivityMember : c.rateActivity;
            } else if (v.inOffPeak) {
                candidate = c.rateActivity; // 非会员仅闲时窗口内（官方文案"非会员用户仅空闲时段x折"）
            }
        } else {
            // 补贴/限时类：全员现行价，会员再降至活动会员价（turbo 0.2→会员 0.1）
            candidate = c.rateActivity;
            if (v.member && c.rateActivityMember > 0 && c.rateActivityMember < candidate)
                candidate = c.rateActivityMember;
        }
        if (candidate > 0 && (v.effective <= 0 || candidate < v.effective)) {
            v.effective = candidate;
            v.activityTag = tag;
        }
    }
    // 会员全天价（discount.member_discount，glm 系）；若比活动价更低则以会员价为准
    if (v.member && c.rateMember > 0 && (v.effective <= 0 || c.rateMember < v.effective)) {
        v.effective = c.rateMember;
        v.activityTag = nullptr; // 走会员价标注，不占活动标签
    }
    v.memberTeaser = !v.member && c.rateMember > 0;
    return v;
}

bool clampMaxMode(const ModelCaps& caps, bool want, long long cfgWindow, long long& outWindow) {
    if (!want || !caps.maxMode) {
        outWindow = caps.cwDefault > 0 ? caps.cwDefault : 200000;
        return false;
    }
    // 在 caps.cwMax 中取 ≤ cfgWindow 的合法值；cfgWindow==0 取最大
    long long best = 0;
    if (!caps.cwMax.empty()) {
        best = caps.cwMax[0];
        for (auto v : caps.cwMax) {
            if (cfgWindow > 0 ? (v <= cfgWindow && v > best) : (v > best)) best = v;
        }
    }
    if (best <= 0) best = 1000000;
    outWindow = best;
    return true;
}

// ---------- 快照 → 增量（累计快照语义） ----------
// 返回 true 表示有增量；delta 为增量文本。快照比旧值短 → 丢弃帧。
bool snapshotDelta(std::string& last, const std::string& snap, std::string& delta, bool allowReplace) {
    if (snap.empty()) return false;
    if (!last.empty() && snap.size() >= last.size() && snap.compare(0, last.size(), last) == 0) {
        if (snap.size() == last.size()) return false;
        std::string d = snap.substr(last.size());
        // 尾部停在半个字符上时先不发出，等下一帧补齐，
        // 保证任何一帧文本都是完整的 UTF-8 字符序列。
        size_t cut = utf8SafeCut(d, d.size());
        if (cut == 0) return false;
        size_t keep = last.size() + cut;
        delta.assign(d, 0, cut);
        last.assign(snap, 0, keep);
        return true;
    }
    // 不以旧值为前缀
    if (!allowReplace && snap.size() < last.size()) return false; // 思考快照变短 → 丢弃
    delta = snap;
    last = snap;
    return true;
}

// ---------- 通道调度 ----------
void logRequestFailure(const char* endpoint, const std::string& model, const Account& acc, const UpResult& r) {
    if (r.ok || r.clientAborted) return;
    std::string reason = r.error.empty() ? "上游未提供错误原因" : r.error;
    reason.resize(utf8SafeCut(reason, 500));
    for (char& ch : reason) if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    LOG_W("请求失败 [%s] 模型=%s 账号=%s HTTP=%d code=%d 原因=%s",
          endpoint, model.c_str(), acc.nickname.c_str(), r.httpStatus, r.code, reason.c_str());
}

// 固定使用 SOLO 通道（/api/agent/v3/llm_utils_chat，chat_v3 主目录模型；
// 原生 function calling、思考档位前缀注入、Max/1M 上下文均在该通道实现）。
UpResult upstreamDispatch(const UpRequest& req, Account& acc, const ModelCaps& caps, const UpSink& sink) {
    UpResult r = upstreamSolo(req, acc, caps, sink);
    // 认证失败 → 刷新令牌后重试一次（握手期失败，尚未向下游发出事件）
    if (!r.ok && !r.clientAborted && (r.code == 1001 || r.httpStatus == 401)) {
        LOG_W("solo 认证失败，刷新令牌后重试: %s", r.error.c_str());
        AccountPool::instance().ensureFreshToken(acc);
        r = upstreamSolo(req, acc, caps, sink);
    }
    return r;
}
