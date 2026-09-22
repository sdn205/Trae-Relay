// AccountPool.h - 多账号池：凭证、积分、并发闸门
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common/Json.h"
#include "accounts/Storage.h"

struct ModelCaps;

struct Account {
    AuthData auth;
    std::string machineId;
    std::string deviceId;
    std::string editionId;     // cn / solo / sg
    std::string nickname;
    std::atomic<double> credits{ -1 };       // -1 = 未知
    std::atomic<int> payIdentity{ -1 };      // ide_user_pay_status.user_pay_identity：-1 未知，0=Free，>0 付费档
    std::atomic<int> active{ 0 };            // 当前并发会话数
    std::atomic<long long> lastUsedTs{ 0 };
    std::atomic<long long> lastRequestTs{ 0 };
    std::mutex refreshMtx;                   // 令牌刷新串行化
    std::mutex gateMtx;
    std::condition_variable gateCv;

    bool tryAcquireSlot(int maxConcurrent) {
        std::unique_lock<std::mutex> lk(gateMtx);
        if (active.load() >= maxConcurrent) return false;
        ++active;
        return true;
    }
    void releaseSlot() {
        std::unique_lock<std::mutex> lk(gateMtx);
        int v = active.load();
        if (v > 0) active.store(v - 1);
        lk.unlock();
        gateCv.notify_all();
    }
};

// 一条使用记录（usage/usage-YYYYMMDD.jsonl 的一行 / 使用记录页的一行）。
// 积分差值口径：请求结束后异步补查积分，"补查前余额 − 补查后余额"=该(批)请求
// 实际消耗；同账号并发多条时差值是合计，merged 标记，无法拆分。
struct UsageRecord {
    time_t ts = 0;
    std::string account;
    std::string model;                       // 模型目录的 configName，统一记录标识
    std::string endpoint;                    // 入站端点：/v1/chat/completions | /v1/responses
    long long in = 0, out = 0, cache = 0;    // token 数（来自上游 token_usage）
    double creditsBefore = 0, creditsAfter = 0, creditsDelta = 0;
    bool creditsKnown = false;               // false = 积分未回填（显示 "--"）
    bool merged = false;                     // true = 差值为多条并发合计
    int ms = 0;                              // 上游耗时
    bool ok = true;
};

class AccountPool {
public:
    // 自动发现 + 解密全部账号
    void autoDiscover();
    std::vector<std::shared_ptr<Account>>& accounts() { return m_accounts; }

    // Whether any account credentials were discovered.
    bool hasUsableAccount() {
        std::lock_guard<std::mutex> lk(m_mtx);
        return !m_accounts.empty();
    }

    // 按积分降序/轮询挑一个可用账号并占用并发槽（阻塞至多 timeoutMs）
    std::shared_ptr<Account> acquire(int timeoutMs);
    // Release the slot and log request failures without suspending the account.
    void release(std::shared_ptr<Account> acc, bool ok, int errorCode);

    // 令牌刷新（过期前 30 分钟触发；串行化）；失败时不动旧 token
    bool ensureFreshToken(Account& acc);
    // 积分刷新（ide_user_ent_usage）；acc 为空则刷全部
    void refreshCredits(Account* acc = nullptr);
    // 异步积分刷新（请求结束后调用，不阻塞响应）：入队合并，
    // 单工作线程串行刷，重复入队去重。积分不随聊天流下发（2026-09-19 探针实测），
    // 请求后补查是唯一近实时口径。
    void refreshCreditsAsync(std::shared_ptr<Account> acc);
    // 从上游 notify_usage / token_usage 事件同步当前账号剩余积分。
    bool updateCreditsFromEvent(Account& acc, const Json& event);
    // 倍率显示用的会员身份共识：任一账号非会员或未知 → 0（保守，请求可能路由到它）；
    // 全部账号均为付费档 → 返回最小档位值（>0）。
    int payIdentityForDisplay();

    // ---- 使用记录（dist/usage/usage-YYYYMMDD.jsonl，一条请求一行）----
    // 请求收尾登记：token 已知，积分差值等积分回填（refreshCredits）时落账。
    // 只在会触发积分补查的出口调用，否则记录永远不会落账。
    // 统一接收解析后的模型，由记录层取 configName，避免混入显示名或请求别名。
    void usageRecordPending(std::shared_ptr<Account> acc, const ModelCaps& model,
                            const std::string& endpoint,
                            long long inTok, long long outTok, long long cacheTok, int ms);
    static constexpr int kUsageHistoryLimit = 100;
    // 分页读取最近 100 条已落账记录：page 0 = 最新 limit 条。
    // hasMore 仅表示这 100 条内还有更早记录；UI 翻页/实时刷新同源。
    std::vector<UsageRecord> usagePage(int page, int limit, bool& hasMore);
    // 已加载记录数（最多 100，分页器算总页数用）。
    int usageTotalCount();
    // 今日汇总独立于分页缓存，包含当天全部已落账记录。
    long long usageCountToday(long long* tokens = nullptr);
    // 退出时把未落账的记录写盘（积分未知口径），防丢条目。
    void usageFlushPending();
    // 每日签到：0=领取成功，1=今日已领取，-1=失败
    int doCheckin(Account& acc);

    static AccountPool& instance();

private:
    AccountPool() = default;
    std::vector<std::shared_ptr<Account>> m_accounts;
    std::atomic<int> m_count{ 0 };
    std::atomic<int> m_rr{ 0 };
    std::mutex m_mtx;
    // 异步积分刷新队列（单工作线程 + 去重合并）
    std::mutex m_creditQueueMtx;
    std::vector<std::shared_ptr<Account>> m_creditQueue;
    std::atomic<bool> m_creditWorkerBusy{ false };
    // 使用记录：待积分回填的请求（按收尾顺序），回填后即落盘
    std::mutex m_usageMtx;
    // 使用记录内存缓存（新→旧，最多 100 条）：从文件尾部按需读取，
    // 落账时插入头部并淘汰最旧项，磁盘记录仍按原有 30 天策略保留。
    std::vector<UsageRecord> m_usageCache;
    bool m_usageCacheLoaded = false;
    time_t m_usageTodayStart = 0, m_usageTodayEnd = 0;
    long long m_usageTodayCount = 0, m_usageTodayTokens = 0;
    void usageEnsureCacheLocked();               // 调用方须持有 m_usageMtx
    void usageAppend(const UsageRecord& r);      // 写盘 + 插入缓存头
    struct UsagePending {
        std::shared_ptr<Account> acc;
        UsageRecord rec;
    };
    std::vector<UsagePending> m_usagePending;
    bool m_usageDirReady = false;
};
