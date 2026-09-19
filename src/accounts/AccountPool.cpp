// AccountPool.cpp
#include "accounts/AccountPool.h"
#include "common/Crypto.h"
#include "common/Http.h"
#include "common/Log.h"
#include "common/Settings.h"
#include "accounts/TokenRefresh.h"
#include "upstream/Upstream.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <ctime>

AccountPool& AccountPool::instance() {
    static AccountPool p;
    return p;
}

void AccountPool::autoDiscover() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_count.load() > 0) return;
    auto editions = Storage::discoverEditions();
    for (auto& ed : editions) {
        auto acc = std::make_shared<Account>();
        std::string err;
        if (!Storage::readAuth(ed.userDir, acc->auth, err)) {
            LOG_W("账号读取失败 [%s]: %s", ed.label.c_str(), err.c_str());
            continue;
        }
        acc->machineId = Storage::readMachineId(ed.userDir);
        // x-device-id 优先用 AHA/TTNet 注册设备 ID（发积分类接口按它校验设备），
        // 读不到再退回 machineid/devDeviceId 兜底。
        acc->deviceId = Storage::readAhaDeviceId(ed.userDir);
        if (acc->deviceId.empty()) acc->deviceId = Storage::readDeviceId(ed.userDir);
        acc->editionId = ed.id;
        acc->nickname = "Trae-" + (acc->auth.userId.size() > 4 ? acc->auth.userId.substr(acc->auth.userId.size() - 4) : acc->auth.userId);
        if (acc->machineId.empty())
            acc->machineId = crypto::sha512Hex(acc->auth.userId + acc->auth.accessToken).substr(0, 32);
        if (acc->deviceId.empty()) acc->deviceId = acc->machineId;
        LOG_I("发现账号 %s（%s）token=%s", acc->nickname.c_str(), ed.label.c_str(),
              logRedact(acc->auth.accessToken).c_str());
        m_accounts.push_back(acc);
        m_count.store((int)m_accounts.size());
    }
    if (m_count.load() == 0) {
        LOG_E("未发现任何可用账号：请确认 Trae CN 已登录（%%APPDATA%%\\Trae CN）");
    }
}

std::shared_ptr<Account> AccountPool::acquire(int timeoutMs) {
    auto cfg = settings::get();
    int maxC = cfg->maxConcurrentPerAccount;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        std::vector<std::shared_ptr<Account>> cands;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            cands = m_accounts;
        }
        // 过滤可用
        std::vector<std::shared_ptr<Account>> avail;
        for (auto& a : cands)
            if (a->available()) avail.push_back(a);
        std::shared_ptr<Account> picked;
        if (!avail.empty()) {
            if (cfg->poolSelectBy == "roundRobin") {
                picked = avail[m_rr.fetch_add(1) % avail.size()];
            } else {
                // 积分降序；未知积分(-1)排后
                std::sort(avail.begin(), avail.end(), [](const std::shared_ptr<Account>& a, const std::shared_ptr<Account>& b) {
                    return a->credits.load() > b->credits.load();
                });
                picked = avail[0];
            }
            // 最小请求间隔
            long long nowMs = (long long)(time(nullptr)) * 1000;
            long long lastMs = picked->lastRequestTs.load();
            int minGap = cfg->minRequestIntervalMs;
            if (minGap > 0 && lastMs > 0 && nowMs - lastMs < minGap) {
                // 换下一个或等待
                for (auto& a : avail) {
                    if (a.get() != picked.get() && nowMs - a->lastRequestTs.load() >= minGap) {
                        picked = a;
                        break;
                    }
                }
            }
            if (picked->tryAcquireSlot(maxC)) {
                picked->lastRequestTs.store((long long)time(nullptr) * 1000);
                return picked;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) return nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void AccountPool::release(std::shared_ptr<Account> acc, bool ok, int errorCode) {
    if (!acc) return;
    if (ok) {
        acc->failCount.store(0);
    } else {
        auto cfg = settings::get();
        int fails = acc->failCount.fetch_add(1) + 1;
        long long now = (long long)time(nullptr);
        long long cd = 0;
        switch (errorCode) {
        case 0: break;
        case 1001: cd = 0; break;             // 认证失败：刷新令牌处理，不冷却
        case 1005: cd = 12 * 3600; break;     // 权益不足：长冷却 12h
        case 4008: cd = 6 * 3600; break;      // 配额超限：等日重置（保守 6h）
        case 4011: cd = cfg->poolCooldownSec; break; // 限流窗口
        case 429:  cd = 60; break;            // 软限流
        case 4001: cd = 0; break;             // 参数问题不冷却账号
        default:
            if (errorCode >= 500) cd = 15;    // 5xx 短冷却重试
            else cd = cfg->poolCooldownSec;
            break;
        }
        if (errorCode != 0 && cd == 0 && errorCode != 1001 && errorCode != 4001) cd = cfg->poolCooldownSec;
        if (cd > 0) acc->cooldownUntil.store(now + cd);
        if (fails >= cfg->poolDisableAfterFails && errorCode != 4001) {
            acc->disabled.store(true);
            acc->disabledAt.store(now);
            LOG_W("账号 %s 连续失败 %d 次，已临时禁用（%llds 后自动恢复）",
                  acc->nickname.c_str(), fails, Account::kDisabledRecoverSec);
        }
        if (errorCode != 0) LOG_W("账号 %s 请求失败 code=%d 冷却 %llds", acc->nickname.c_str(), errorCode, cd);
    }
    acc->lastUsedTs.store((long long)time(nullptr));
    acc->releaseSlot();
}

bool AccountPool::ensureFreshToken(Account& acc) {
    std::lock_guard<std::mutex> lk(acc.refreshMtx);
    long long now = (long long)time(nullptr);
    if (!token::needsRefresh(acc.auth, now)) {
        // 未知过期时间且从未验证过：尝试刷新一次（fail-safe）
        if (acc.auth.expiredTs != 0) return true;
    }
    auto rr = token::exchangeToken(acc.auth);
    if (!rr.ok) {
        LOG_W("账号 %s 令牌刷新失败: %s", acc.nickname.c_str(), rr.error.c_str());
        return acc.auth.expiredTs == 0 || acc.auth.expiredTs > now; // 刷新失败但旧 token 可能仍有效
    }
    acc.auth = rr.auth;
    return true;
}

// 从响应 JSON 任意层级找积分字段。当前接口的权威口径是 usage_summary，
// total_amount - consumed_amount；entitlement pack 只作为旧版本回退。
static bool findUsageSummaryDeep(const Json& j, double& credits, int depth = 0) {
    if (depth > 8) return false;
    if (j.isObject()) {
        const Json* summary = j.find("usage_summary");
        if (summary && summary->isObject()) {
            const Json* remaining = summary->find("remaining_amount");
            if (!remaining) remaining = summary->find("available_amount");
            auto number = [](const Json* v, double& out) {
                if (!v) return false;
                if (v->isNumber()) { out = v->asDouble(0); return true; }
                if (v->isString()) {
                    char* end = nullptr;
                    double d = strtod(v->asString().c_str(), &end);
                    if (end && end != v->asString().c_str() && *end == '\0') { out = d; return true; }
                }
                return false;
            };
            double value = 0;
            if (number(remaining, value)) {
                credits = std::max(0.0, value);
                return true;
            }
            const Json* total = summary->find("total_amount");
            const Json* consumed = summary->find("consumed_amount");
            double totalValue = 0, consumedValue = 0;
            if (number(total, totalValue) && number(consumed, consumedValue)) {
                credits = std::max(0.0, totalValue - consumedValue);
                return true;
            }
        }
        for (auto& m : j.members())
            if (findUsageSummaryDeep(m.second, credits, depth + 1)) return true;
    } else if (j.isArray()) {
        for (size_t i = 0; i < j.size(); ++i)
            if (findUsageSummaryDeep(j.at(i), credits, depth + 1)) return true;
    }
    return false;
}

static bool findCreditsEventDeep(const Json& j, double& credits, int depth = 0) {
    if (depth > 10) return false;
    auto number = [](const Json* v, double& out) {
        if (!v) return false;
        if (v->isNumber()) { out = v->asDouble(0); return true; }
        if (v->isString()) {
            char* end = nullptr;
            double d = strtod(v->asString().c_str(), &end);
            if (end && end != v->asString().c_str() && *end == '\0') { out = d; return true; }
        }
        return false;
    };
    if (j.isObject()) {
        for (auto key : { "cn_credits_remain_info", "credits_remain_info", "creditsRemainInfo" }) {
            const Json* info = j.find(key);
            if (!info || !info->isObject()) continue;
            for (auto field : { "ide_credits", "ideCredits", "remaining_amount", "available_amount" }) {
                double value = 0;
                if (number(info->find(field), value)) {
                    credits = std::max(0.0, value);
                    return true;
                }
            }
        }
        for (auto key : { "ide_credits", "ideCredits" }) {
            double value = 0;
            if (number(j.find(key), value)) {
                credits = std::max(0.0, value);
                return true;
            }
        }
        for (auto& m : j.members())
            if (findCreditsEventDeep(m.second, credits, depth + 1)) return true;
    } else if (j.isArray()) {
        for (size_t i = 0; i < j.size(); ++i)
            if (findCreditsEventDeep(j.at(i), credits, depth + 1)) return true;
    }
    return false;
}

static bool findCreditsFromPacks(const Json& j, double& credits, int depth = 0) {
    if (depth > 8) return false;
    if (j.isObject()) {
        const Json* pack = j.find("user_entitlement_pack_list");
        if (pack && pack->isArray() && pack->size() > 0) {
            double sum = 0;
            bool found = false;
            long long now = static_cast<long long>(time(nullptr));
            for (size_t i = 0; i < pack->size(); ++i) {
                const Json& item = pack->at(i);
                const Json* base = item.find("entitlement_base_info");
                if (!base || !base->isObject()) continue;
                long long expiry = item.get("expire_time", Json((double)0)).asInt(0);
                if (!expiry) expiry = base->get("end_time", Json((double)0)).asInt(0);
                if (expiry > 0 && expiry <= now) continue;
                const Json* quota = base->find("quota");
                const Json* usage = item.find("usage");
                if (!usage) usage = base->find("usage");
                if (!quota || !quota->isObject()) continue;
                double limit = quota->get("credits_limit", Json((double)-1)).asDouble(-1);
                if (limit < 0) continue; // 权益包，不是积分包
                double used = usage && usage->isObject()
                    ? usage->get("credits_amount", Json((double)0)).asDouble(0) : 0;
                sum += std::max(0.0, limit - used);
                found = true;
            }
            if (found) {
                credits = sum;
                return true;
            }
        }
        for (auto& m : j.members())
            if (findCreditsFromPacks(m.second, credits, depth + 1)) return true;
    } else if (j.isArray()) {
        for (size_t i = 0; i < j.size(); ++i)
            if (findCreditsFromPacks(j.at(i), credits, depth + 1)) return true;
    }
    return false;
}

// 付费档位显示名（ide_user_pay_status.user_pay_identity，官方枚举）
static const char* payIdentityName(int id) {
    switch (id) {
        case 0: return "Free";
        case 1: return "Pro";
        case 2: return "Pro+";
        case 3: return "Ultra";
        case 4: return "Trial";
        case 5: return "Lite";
        case 100: return "Express";
        default: return "未知";
    }
}

// 拉会员身份（/trae/api/v2/pay/ide_user_pay_status）。倍率显示按它门控：
// 0=Free 按原价，>0（Pro/Pro+/Ultra/Trial/Lite/Express）享会员价。随积分一并实时刷新。
static void refreshPayIdentity(Account* acc) {
    if (!acc) return;
    std::string host = "https://api.trae.cn";
    if (!acc->auth.host.empty() && acc->auth.host.find("http") == 0) host = acc->auth.host;
    while (host.size() && host.back() == '/') host.pop_back();
    Json body = Json::object();
    body.set("trae_client", Json("IDE"));
    body.set("device_id", Json(acc->deviceId.empty() ? acc->machineId : acc->deviceId));
    auto cfg = settings::get();
    auto resp = http::send("POST", host + "/trae/api/v2/pay/ide_user_pay_status",
                           ideHeaders(*acc, cfg->ideVersion, cfg->ideVersionCode, false),
                           body.dump(), 15000);
    if (!resp.ok()) {
        LOG_W("账号 %s 会员身份查询失败: HTTP %d", acc->nickname.c_str(), resp.status);
        return;
    }
    Json j;
    if (!Json::parse(resp.body, j)) return;
    // code != 0 时上游通常不给 identity 字段，get 兜底 -1 保持旧值
    int oldId = acc->payIdentity.load();
    const Json* idv = j.find("user_pay_identity");
    if (!idv || !idv->isNumber()) return;
    int id = (int)idv->asDouble(-1);
    if (id < 0) return;
    acc->payIdentity.store(id);
    if (id != oldId)
        LOG_I("账号 %s 会员身份: %s(%d)（原 %s(%d)）", acc->nickname.c_str(),
              payIdentityName(id), id, payIdentityName(oldId), oldId);
}

static void usageAppendLine(const UsageRecord& r);

void AccountPool::refreshCredits(Account* acc) {
    std::vector<std::shared_ptr<Account>> targets;
    if (acc) {
        targets.push_back(std::shared_ptr<Account>(acc, [](Account*) {}));
    } else {
        std::lock_guard<std::mutex> lk(m_mtx);
        targets = m_accounts;
    }
    for (auto& a : targets) {
        if (!ensureFreshToken(*a)) continue;
        refreshPayIdentity(a.get());
        std::string host = "https://api.trae.cn";
        if (!a->auth.host.empty() && a->auth.host.find("http") == 0) host = a->auth.host;
        while (host.size() && host.back() == '/') host.pop_back();
        // 使用记录口径：此刻余额即"补查前余额"，与新余额相减=这(批)请求消耗
        double beforeCredits = a->credits.load();
        Json body = Json::object();
        body.set("require_usage", Json(true));
        body.set("req_source", Json(0));
        auto cfg = settings::get();
        auto resp = http::send("POST", host + "/trae/api/v2/pay/ide_user_ent_usage",
                               ideHeaders(*a, cfg->ideVersion, cfg->ideVersionCode, false),
                               body.dump(), 20000);
        if (!resp.ok()) {
            LOG_W("账号 %s 积分查询失败: HTTP %d", a->nickname.c_str(), resp.status);
            continue;
        }
        Json j;
        if (!Json::parse(resp.body, j)) continue;
        double credits = -1;
        if (findUsageSummaryDeep(j, credits) || findCreditsEventDeep(j, credits) || findCreditsFromPacks(j, credits)) {
            double old = a->credits.exchange(credits);
            LOG_I("账号 %s 积分: %.1f（原 %.1f）", a->nickname.c_str(), credits, old);
            // 使用记录落账：弹出该账号最老一条待结请求，差值=回填前-回填后
            std::vector<UsageRecord> toWrite;
            {
                std::lock_guard<std::mutex> lk(m_usageMtx);
                auto isTarget = [&](const UsagePending& p) { return p.acc.get() == a.get(); };
                size_t count = 0;
                for (auto& p : m_usagePending)
                    if (isTarget(p)) count++;
                if (count > 0) {
                    for (auto it = m_usagePending.begin(); it != m_usagePending.end(); ++it) {
                        if (!isTarget(*it)) continue;
                        it->rec.creditsBefore = beforeCredits;
                        it->rec.creditsAfter = credits;
                        it->rec.creditsKnown = beforeCredits >= 0;
                        it->rec.merged = count > 1;
                        if (it->rec.creditsKnown)
                            it->rec.creditsDelta = beforeCredits - credits;
                        toWrite.push_back(it->rec);
                        LOG_I("账号 %s 使用记录: %s 输入 %lld 输出 %lld 缓存 %lld 积分 %s%.2f 耗时 %dms%s",
                              a->nickname.c_str(), it->rec.model.c_str(), it->rec.in, it->rec.out,
                              it->rec.cache, it->rec.creditsDelta >= 0 ? "-" : "?",
                              it->rec.creditsKnown ? it->rec.creditsDelta : 0.0, it->rec.ms,
                              it->rec.merged ? "（含并发合计）" : "");
                        m_usagePending.erase(it);
                        break;
                    }
                    // 其余并发待结：差值无法拆分，按未知落盘
                    for (auto it = m_usagePending.begin(); it != m_usagePending.end();) {
                        if (isTarget(*it)) {
                            it->rec.creditsKnown = false;
                            toWrite.push_back(it->rec);
                            it = m_usagePending.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
            }
            for (auto& r : toWrite) usageAppend(r);
        } else {
            LOG_W("账号 %s 积分响应缺少 usage_summary / entitlement pack", a->nickname.c_str());
        }
    }
}

void AccountPool::refreshCreditsAsync(std::shared_ptr<Account> acc) {
    if (!acc) return;
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(m_creditQueueMtx);
        // 已在队列中的账号不重复入队（并发收尾合并为一次查询）
        for (auto& a : m_creditQueue)
            if (a.get() == acc.get()) return;
        m_creditQueue.push_back(std::move(acc));
        start = !m_creditWorkerBusy.exchange(true);
    }
    if (!start) return;
    std::thread([this] {
        for (;;) {
            std::vector<std::shared_ptr<Account>> batch;
            {
                std::lock_guard<std::mutex> lk(m_creditQueueMtx);
                batch.swap(m_creditQueue);
            }
            if (!batch.empty()) {
                for (auto& a : batch) {
                    if (a) refreshCredits(a.get());
                }
                continue;
            }
            // 队列已空：加锁复核后退出，堵住"生产者入队与 worker 退出竞态"窗口
            std::lock_guard<std::mutex> lk(m_creditQueueMtx);
            if (m_creditQueue.empty()) {
                m_creditWorkerBusy = false;
                return;
            }
        }
    }).detach();
}

bool AccountPool::updateCreditsFromEvent(Account& acc, const Json& event) {
    double credits = -1;
    if (findUsageSummaryDeep(event, credits) || findCreditsEventDeep(event, credits)) {
        double old = acc.credits.exchange(credits);
        LOG_I("账号 %s 用量事件积分: %.1f（原 %.1f）", acc.nickname.c_str(), credits, old);
        return true;
    }
    return false;
}

int AccountPool::payIdentityForDisplay() {
    std::lock_guard<std::mutex> lk(m_mtx);
    int consensus = -1;
    for (auto& a : m_accounts) {
        int id = a->payIdentity.load();
        if (id <= 0) return 0; // 非会员或未知：保守按非会员显示（请求可能路由到它）
        consensus = consensus < 0 ? id : (id < consensus ? id : consensus);
    }
    return consensus > 0 ? consensus : 0;
}

// ---------- 使用记录（dist/usage/usage-YYYYMMDD.jsonl，一条请求一行） ----------
static std::wstring usageWiden(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string usageDirPath() { return exeDir() + "\\usage"; }

static std::string usageJsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

static void usageAppendLine(const UsageRecord& r) {
    CreateDirectoryW(usageWiden(usageDirPath()).c_str(), nullptr); // 已存在时静默失败
    char tsbuf[32]{};
    struct tm local {};
    localtime_s(&local, &r.ts);
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &local);
    char line[1024];
    int n = snprintf(line, sizeof(line),
                     "{\"ts\":\"%s\",\"account\":\"%s\",\"model\":\"%s\",\"ep\":\"%s\","
                     "\"in\":%lld,\"out\":%lld,"
                     "\"cache\":%lld,\"before\":%.2f,\"after\":%.2f,\"delta\":%.4f,"
                     "\"known\":%s,\"merged\":%s,\"ms\":%d,\"ok\":%s}\n",
                     tsbuf, usageJsonEscape(r.account).c_str(), usageJsonEscape(r.model).c_str(),
                     usageJsonEscape(r.endpoint).c_str(),
                     r.in, r.out, r.cache, r.creditsBefore, r.creditsAfter, r.creditsDelta,
                     r.creditsKnown ? "true" : "false", r.merged ? "true" : "false",
                     r.ms, r.ok ? "true" : "false");
    if (n <= 0) return;
    char day[16]{};
    strftime(day, sizeof(day), "%Y%m%d", &local);
    FILE* f = _wfopen(usageWiden(usageDirPath() + "\\usage-" + day + ".jsonl").c_str(), L"ab");
    if (!f) {
        LOG_W("使用记录写入失败: %s", (usageDirPath() + "\\usage-" + day + ".jsonl").c_str());
        return;
    }
    fwrite(line, 1, (size_t)n, f);
    fclose(f);
}

// 时间戳展示口径与 jsonl 一致（本地时间）
static bool usageParseTs(const std::string& s, time_t& out) {
    int y, mo, d, h, mi, sec;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return false;
    struct tm local {};
    local.tm_year = y - 1900;
    local.tm_mon = mo - 1;
    local.tm_mday = d;
    local.tm_hour = h;
    local.tm_min = mi;
    local.tm_sec = sec;
    out = mktime(&local);
    return out != -1;
}

void AccountPool::usageRecordPending(std::shared_ptr<Account> acc, const ModelCaps& model,
                                     const std::string& endpoint,
                                     long long inTok, long long outTok, long long cacheTok, int ms) {
    if (!acc) return;
    UsageRecord r;
    r.ts = time(nullptr);
    r.account = acc->nickname;
    r.model = model.configName;
    r.endpoint = endpoint;
    r.in = inTok;
    r.out = outTok;
    r.cache = cacheTok;
    r.ms = ms;
    std::lock_guard<std::mutex> lk(m_usageMtx);
    m_usagePending.push_back({ std::move(acc), std::move(r) });
}

// 积分回填成功后调用：把该账号最老一条待结记录落账（差值=回填前余额-新余额）。
// 每次成功请求恰好触发一次补查，串行场景即单条精确值；多条待结说明并发，
// 合计差值记在最老一条上（merged 标记），其余按未知落盘——宁可显示 "--" 不拆谎。
// （逻辑内联在 refreshCredits 里，需访问私有待结队列）

void AccountPool::usageFlushPending() {
    std::vector<UsageRecord> toWrite;
    {
        std::lock_guard<std::mutex> lk(m_usageMtx);
        for (auto& p : m_usagePending) {
            p.rec.creditsKnown = false;
            toWrite.push_back(p.rec);
        }
        m_usagePending.clear();
    }
    for (auto& r : toWrite) usageAppend(r);
}

// 单行 jsonl -> UsageRecord（格式见 usageAppendLine）
static bool usageParseLine(const std::string& line, UsageRecord& r) {
    if (line.empty()) return false;
    Json j;
    if (!Json::parse(line, j) || !j.isObject()) return false;
    r.ts = 0;
    usageParseTs(j.get("ts", Json("")).asString(), r.ts);
    r.account = j.get("account", Json("")).asString();
    r.model = j.get("model", Json("")).asString();
    r.endpoint = j.get("ep", Json("")).asString();
    r.in = j.get("in", Json((double)0)).asInt(0);
    r.out = j.get("out", Json((double)0)).asInt(0);
    r.cache = j.get("cache", Json((double)0)).asInt(0);
    r.creditsBefore = j.get("before", Json((double)0)).asDouble(0);
    r.creditsAfter = j.get("after", Json((double)0)).asDouble(0);
    r.creditsDelta = j.get("delta", Json((double)0)).asDouble(0);
    r.creditsKnown = j.get("known", Json(false)).asBool(false);
    r.merged = j.get("merged", Json(false)).asBool(false);
    r.ms = (int)j.get("ms", Json((double)0)).asDouble(0);
    r.ok = j.get("ok", Json(true)).asBool(true);
    return true;
}

// 从尾部分块读取，达到上限即停止，不把整个历史文件装入内存。
static void usageReadRecent(FILE* file, std::vector<UsageRecord>& rows, size_t limit) {
    if (_fseeki64(file, 0, SEEK_END) != 0) return;
    auto end = _ftelli64(file);
    std::string reversed;
    bool oversized = false;
    const auto appendLine = [&] {
        if (!oversized && !reversed.empty()) {
            std::reverse(reversed.begin(), reversed.end());
            UsageRecord record;
            if (usageParseLine(reversed, record)) rows.push_back(std::move(record));
        }
        reversed.clear();
        oversized = false;
    };
    char block[4096];
    while (end > 0 && rows.size() < limit) {
        const auto start = std::max<__int64>(0, end - static_cast<__int64>(sizeof(block)));
        if (_fseeki64(file, start, SEEK_SET) != 0) return;
        const size_t length = fread(block, 1, static_cast<size_t>(end - start), file);
        if (!length) return;
        for (size_t i = length; i > 0 && rows.size() < limit; --i) {
            const char c = block[i - 1];
            if (c == '\n') appendLine();
            else if (c != '\r' && !oversized) {
                // 与顺序读取的 2048 字节行缓冲保持一致，损坏长行不会膨胀内存。
                if (reversed.size() < 2047) reversed.push_back(c);
                else oversized = true;
            }
        }
        end = start;
    }
    if (end == 0 && rows.size() < limit) appendLine();
}

// 调用方须持有 m_usageMtx。首次访问只加载最新 100 条，并沿用 30 天过期清理。
void AccountPool::usageEnsureCacheLocked() {
    if (m_usageCacheLoaded) return;
    m_usageCacheLoaded = true;
    std::wstring dir = usageWiden(usageDirPath());
    {
        time_t now = time(nullptr);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\usage-*.jsonl").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = fd.cFileName;
                if (name.size() == 20) { // usage-YYYYMMDD.jsonl
                    struct tm t {};
                    int y, mo, d;
                    if (swscanf(name.c_str(), L"usage-%04d%02d%02d", &y, &mo, &d) == 3) {
                        t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
                        if (now - mktime(&t) > 30LL * 86400)
                            DeleteFileW((dir + L"\\" + name).c_str());
                    }
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\usage-*.jsonl").c_str(), &fd);
    std::vector<std::wstring> files;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.rbegin(), files.rend()); // 文件名含日期，字典序倒序=新→旧
    m_usageCache.reserve(kUsageHistoryLimit);
    for (auto& name : files) {
        if (m_usageCache.size() >= kUsageHistoryLimit) break;
        FILE* f = _wfopen((dir + L"\\" + name).c_str(), L"rb");
        if (!f) continue;
        usageReadRecent(f, m_usageCache, kUsageHistoryLimit);
        fclose(f);
    }
}

// 落账一条：写盘 + 插入缓存头（分页第 0 页 = 最新记录，落账即可见）
void AccountPool::usageAppend(const UsageRecord& r) {
    std::lock_guard<std::mutex> lk(m_usageMtx);
    // 写盘与汇总/缓存更新同锁，避免首次读取与落账并发时重复计数。
    usageAppendLine(r);
    if (m_usageCacheLoaded) {
        if (m_usageCache.size() == kUsageHistoryLimit) m_usageCache.pop_back();
        m_usageCache.insert(m_usageCache.begin(), r);
    }
    if (r.ts >= m_usageTodayStart && r.ts < m_usageTodayEnd) {
        ++m_usageTodayCount;
        m_usageTodayTokens += r.in + r.out + r.cache;
    }
}

int AccountPool::usageTotalCount() {
    std::lock_guard<std::mutex> lk(m_usageMtx);
    usageEnsureCacheLocked();
    return (int)m_usageCache.size();
}

long long AccountPool::usageCountToday(long long* tokens) {
    std::lock_guard<std::mutex> lk(m_usageMtx);
    const time_t now = time(nullptr);
    struct tm local {};
    localtime_s(&local, &now);
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    const time_t start = mktime(&local);
    ++local.tm_mday;
    local.tm_isdst = -1;
    const time_t end = mktime(&local);
    if (m_usageTodayStart != start) {
        m_usageTodayStart = start;
        m_usageTodayEnd = end;
        m_usageTodayCount = 0;
        m_usageTodayTokens = 0;
        // 首次访问或跨日时逐行汇总当天文件，只保存计数，不保留所有记录对象。
        localtime_s(&local, &now);
        char day[16]{};
        strftime(day, sizeof(day), "%Y%m%d", &local);
        FILE* file = _wfopen(usageWiden(usageDirPath() + "\\usage-" + day + ".jsonl").c_str(), L"rb");
        if (file) {
            char line[2048];
            while (fgets(line, sizeof(line), file)) {
                UsageRecord record;
                if (!usageParseLine(line, record) || record.ts < start || record.ts >= end) continue;
                ++m_usageTodayCount;
                m_usageTodayTokens += record.in + record.out + record.cache;
            }
            fclose(file);
        }
    }
    if (tokens) *tokens = m_usageTodayTokens;
    return m_usageTodayCount;
}

std::vector<UsageRecord> AccountPool::usagePage(int page, int limit, bool& hasMore) {
    hasMore = false;
    if (page < 0 || limit <= 0) return {};
    std::lock_guard<std::mutex> lk(m_usageMtx);
    usageEnsureCacheLocked();
    size_t skip = static_cast<size_t>(page) * limit;
    if (skip >= m_usageCache.size()) return {};
    size_t end = std::min(m_usageCache.size(), (size_t)skip + limit);
    hasMore = end < m_usageCache.size();
    return std::vector<UsageRecord>(m_usageCache.begin() + skip, m_usageCache.begin() + end);
}

static bool findCheckinFlagDeep(const Json& j, bool& value, int depth = 0) {
    if (depth > 8) return false;
    auto parse = [](const Json* v, bool& out) {
        if (!v) return false;
        if (v->isBool()) { out = v->asBool(false); return true; }
        if (v->isNumber()) { out = v->asInt(0) != 0; return true; }
        if (v->isString()) {
            std::string s = v->asString();
            for (auto& c : s) c = (char)tolower((unsigned char)c);
            if (s == "true" || s == "1" || s == "yes" || s == "already" || s == "checked_in") { out = true; return true; }
            if (s == "false" || s == "0" || s == "no") { out = false; return true; }
        }
        return false;
    };
    if (j.isObject()) {
        // status 接口同时返回 checked_in（可签到资格）和
        // did_checked_in（今天是否已领）。后者必须优先，否则会把
        // 可签到账号误判为已签到，导致永远不调用 claim。
        for (auto key : { "did_checked_in", "didCheckedIn" }) {
            bool parsed = false;
            if (parse(j.find(key), parsed)) { value = parsed; return true; }
        }
        for (auto key : { "already_checked_in", "is_checked_in", "has_checked_in",
                          "isCheckIn", "checkedIn", "claimed", "alreadyClaimed",
                          "checkin_status", "checkinStatus" }) {
            bool parsed = false;
            if (parse(j.find(key), parsed)) { value = parsed; return true; }
        }
        bool found = false;
        for (auto& m : j.members()) {
            bool child = false;
            if (findCheckinFlagDeep(m.second, child, depth + 1)) {
                value = child;
                return true;
            }
        }
        // checked_in 仅作为老版本没有 did_checked_in 的最后回退。
        bool checked = false;
        if (parse(j.find("checked_in"), checked)) { value = checked; return true; }
        return found;
    }
    if (j.isArray()) {
        for (size_t i = 0; i < j.size(); ++i) {
            bool child = false;
            if (findCheckinFlagDeep(j.at(i), child, depth + 1)) { value = child; return true; }
        }
    }
    return false;
}

static bool findCodeDeep(const Json& j, int& code, int depth = 0) {
    if (depth > 8) return false;
    if (j.isObject()) {
        bool found = false;
        for (auto key : { "code", "status_code", "error_code" }) {
            const Json* v = j.find(key);
            if (!v) continue;
            int candidate = 0;
            bool parsed = false;
            if (v->isNumber()) { candidate = (int)v->asInt(0); parsed = true; }
            if (v->isString()) {
                char* end = nullptr;
                long value = strtol(v->asString().c_str(), &end, 10);
                if (end && *end == '\0') { candidate = (int)value; parsed = true; }
            }
            if (!parsed) continue;
            found = true;
            // 业务错误码优先于同一响应外层的 code=0 包装。
            if (candidate != 0) { code = candidate; return true; }
        }
        for (auto& m : j.members()) {
            int nested = 0;
            if (findCodeDeep(m.second, nested, depth + 1)) {
                if (nested != 0) { code = nested; return true; }
                found = true;
            }
        }
        if (found) { code = 0; return true; }
    } else if (j.isArray()) {
        bool found = false;
        int zero = 0;
        for (size_t i = 0; i < j.size(); ++i) {
            int nested = 0;
            if (findCodeDeep(j.at(i), nested, depth + 1)) {
                found = true;
                if (nested != 0) { code = nested; return true; }
                zero = nested;
            }
        }
        if (found) { code = zero; return true; }
    }
    return false;
}

int AccountPool::doCheckin(Account& acc) {
    if (!ensureFreshToken(acc)) return -1;
    std::string host = "https://api.trae.cn";
    if (!acc.auth.host.empty() && acc.auth.host.find("http") == 0) host = acc.auth.host;
    while (host.size() && host.back() == '/') host.pop_back();
    auto cfg = settings::get();
    auto hdr = ideHeaders(acc, cfg->ideVersion, cfg->ideVersionCode, false);
    hdr.set("x-device-id", acc.deviceId.empty() ? acc.machineId : acc.deviceId);
    Json request = Json::object();
    request.set("req_source", Json(acc.editionId == "work" ? 2 : 1));
    auto st = http::send("POST", host + "/trae/api/v2/ug/checkin_credits/status", hdr, request.dump(), 15000);
    Json j;
    if (!st.ok() || !Json::parse(st.body, j)) {
        LOG_W("账号 %s 签到状态请求失败: HTTP %d err=%s", acc.nickname.c_str(), st.status, st.error.c_str());
        return -1;
    }
    bool checked = false;
    int statusCode = 0;
    if (findCheckinFlagDeep(j, checked) && checked) {
            LOG_I("账号 %s 今日已签到", acc.nickname.c_str());
            return 1;
    }
    if (findCodeDeep(j, statusCode) && statusCode == 9095) return 1;

    // 9074「当前参与用户太多」是上游签到活动的并发限流，同一份请求换个时段就能成功
    // （2026-09-19 实测：设计文档既定结论 9074→重试），退避重试等待窗口；
    // 其余业务码维持一次性失败。
    static constexpr int kClaimRetries = 5;
    static constexpr int kRetryDelaySec[kClaimRetries] = { 15, 30, 45, 60, 60 };
    for (int attempt = 0;; ++attempt) {
        auto cl = http::send("POST", host + "/trae/api/v2/ug/checkin_credits/claim", hdr, request.dump(), 15000);
        if (cl.status != 200) {
            LOG_W("账号 %s 签到请求失败: HTTP %d", acc.nickname.c_str(), cl.status);
            return -1;
        }
        bool parsed = Json::parse(cl.body, j);
        int code = 0;
        bool codeFound = parsed && findCodeDeep(j, code);
        if (parsed && codeFound && code == 9074 && attempt < kClaimRetries) {
            LOG_W("账号 %s 签到限流(9074)，%ds 后重试(%d/%d)", acc.nickname.c_str(),
                  kRetryDelaySec[attempt], attempt + 1, kClaimRetries);
            std::this_thread::sleep_for(std::chrono::seconds(kRetryDelaySec[attempt]));
            continue;
        }
        bool already = codeFound && code == 9095;
        bool flag = false;
        bool flagFound = parsed && findCheckinFlagDeep(j, flag);
        // 官方 claim：HTTP 200 且无业务码（或 code=0）才算成功。
        // 响应不是合法 JSON 时绝不能误判成功（曾把非 JSON 的 200 拦截页
        // 当成签到成功，积分实际一分没到账）。
        bool success = parsed && (!codeFound || code == 0);
        if (flagFound && flag) success = true;
        if (success) {
            LOG_I("账号 %s 签到成功", acc.nickname.c_str());
            refreshCredits(&acc);
            return 0;
        } else if (already) {
            LOG_I("账号 %s 今日已签到（9095）", acc.nickname.c_str());
            return 1;
        } else {
            LOG_W("账号 %s 签到失败 code=%d body=%s", acc.nickname.c_str(), code,
                  cl.body.substr(0, 200).c_str());
            return -1;
        }
    }
}
