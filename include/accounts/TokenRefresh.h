// TokenRefresh.h - ExchangeToken 令牌刷新
#pragma once
#include "accounts/Storage.h"

namespace token {

struct RefreshResult {
    bool ok = false;
    std::string error;
    AuthData auth;    // 刷新成功时返回新凭证（含新 refreshToken/expiry）
};

// 用 refreshToken 换新 accessToken。host 优先用 auth.host，否则 https://api.trae.cn
RefreshResult exchangeToken(const AuthData& auth);

// 判定是否需要刷新（提前 30 分钟）；expiredTs==0 视为未知（需要时调用方自行决定）
bool needsRefresh(const AuthData& auth, long long nowSec);

} // namespace token
