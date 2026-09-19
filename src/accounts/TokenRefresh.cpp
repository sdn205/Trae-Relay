// TokenRefresh.cpp - ExchangeToken / 过期判定
#include "accounts/TokenRefresh.h"
#include "common/Http.h"
#include "common/Json.h"
#include "common/Log.h"
#include <ctime>

namespace token {

bool needsRefresh(const AuthData& auth, long long nowSec) {
    if (auth.refreshToken.empty()) return false;
    if (auth.expiredTs == 0) return false; // 未知过期时间，不主动刷
    return nowSec >= auth.expiredTs - 1800;
}

static std::string findStrDeep(const Json& j, std::initializer_list<const char*> keys, int depth = 0) {
    if (depth > 4) return "";
    for (auto k : keys) {
        const Json* v = j.find(k);
        if (v && v->isString() && !v->asString().empty()) return v->asString();
    }
    if (j.isObject()) {
        for (auto& m : j.members()) {
            if (m.second.isObject() || m.second.isArray()) {
                std::string r = findStrDeep(m.second, keys, depth + 1);
                if (!r.empty()) return r;
            }
        }
    }
    return "";
}

RefreshResult exchangeToken(const AuthData& auth) {
    RefreshResult rr;
    if (auth.refreshToken.empty()) {
        rr.error = "无 refreshToken，无法刷新";
        return rr;
    }
    std::string host = auth.host.empty() ? "https://api.trae.cn" : auth.host;
    // CN 版本 host 若指向 mchost.guru 则强制走 api.trae.cn（网关不做 OAuth）
    if (host.find("mchost.guru") != std::string::npos) host = "https://api.trae.cn";
    while (host.size() && host.back() == '/') host.pop_back();
    std::string url = host + "/cloudide/api/v3/trae/oauth/ExchangeToken";

    Json body = Json::object();
    body.set("ClientID", Json(auth.clientId.empty() ? "ono9krqynydwx5" : auth.clientId));
    body.set("RefreshToken", Json(auth.refreshToken));
    body.set("ClientSecret", Json("-"));
    body.set("UserID", Json(auth.userId));

    http::Headers hd;
    hd.set("Content-Type", "application/json");
    auto resp = http::send("POST", url, hd, body.dump(), 30000);
    if (resp.status == 0) {
        rr.error = "网络错误: " + resp.error;
        return rr;
    }
    if (resp.status != 200) {
        rr.error = "HTTP " + std::to_string(resp.status) + ": " + resp.body.substr(0, 300);
        return rr;
    }
    Json j;
    std::string perr;
    if (!Json::parse(resp.body, j, &perr)) {
        rr.error = "响应非法 JSON";
        return rr;
    }
    const Json* errMeta = j.find("ResponseMetadata");
    if (errMeta && errMeta->isObject()) {
        const Json* e = errMeta->find("Error");
        if (e && e->isObject()) {
            const Json* code = e->find("Code");
            bool hasCode = code && !code->isNull();
            const Json* msg = e->find("Message");
            if (hasCode || (msg && !msg->isNull())) {
                rr.error = "上游错误: " + e->dump().substr(0, 300);
                return rr;
            }
        }
    }
    const Json* result = j.find("Result");
    if (!result) result = j.find("result");
    const Json& src = (result && result->isObject()) ? *result : j;

    std::string newToken = findStrDeep(src, { "Token", "token", "AccessToken", "accessToken" });
    if (newToken.empty()) {
        rr.error = "响应缺 Token: " + resp.body.substr(0, 300);
        return rr;
    }
    rr.auth = auth;
    rr.auth.accessToken = newToken;
    std::string newRt = findStrDeep(src, { "RefreshToken", "refreshToken" });
    if (!newRt.empty()) rr.auth.refreshToken = newRt;
    std::string exp = findStrDeep(src, { "TokenExpireAt", "tokenExpireAt", "expiredAt" });
    if (!exp.empty()) {
        rr.auth.expiredRaw = exp;
        rr.auth.expiredTs = Storage::parseExpiry(exp);
    }
    // userInfo 补充 psd
    const Json* ui = src.find("UserInfo");
    if (!ui) ui = src.find("userInfo");
    if (ui && ui->isString() && !ui->asString().empty()) {
        Json uij;
        if (Json::parse(ui->asString(), uij) && uij.isObject()) {
            // 合并进 raw 便于 psd 读取
            rr.auth.raw.mergeMissing(uij);
        }
    }
    rr.ok = true;
    LOG_I("令牌刷新成功 uid=%s", logRedact(auth.userId).c_str());
    return rr;
}

} // namespace token
