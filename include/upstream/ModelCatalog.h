// ModelCatalog.h - 模型配置表缓存（get_detail_param 主目录，stale-while-revalidate）
#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "common/Json.h"
#include "upstream/Upstream.h"

class Account;

class ModelCatalog {
public:
    // 取全部模型能力（5 分钟新鲜窗口）。
    // 语义（stale-while-revalidate，绝不长时间阻塞调用线程）：
    //  - 新鲜：直接返回缓存；
    //  - 过期但曾成功：立即返回旧缓存，并保证只有一个后台线程去刷新；
    //  - 从未成功：本次同步刷新（首启 / 上游长期不可用场景）。
    std::vector<ModelCaps> all(Account& acc, std::string& err);
    // 单模型解析（all 的薄封装）
    bool get(Account& acc, const std::string& configName, ModelCaps& out, std::string& err);

    // ---- 纯内存访问（UI 线程用，永不发网络请求）----
    bool ready() const { return m_everOk.load(); }
    int version() const { return m_version.load(); }
    std::vector<ModelCaps> cached() const;
    // 保证至多一个后台刷新在跑；可在任意线程调用
    void triggerRefreshAsync();

    static ModelCatalog& instance();

private:
    // 一次完整拉取 + 合并 + 提交；成功更新缓存并自增 version
    bool refreshOnce(Account& acc, std::string& err);

    mutable std::mutex m_mtx;
    std::vector<ModelCaps> m_cache;
    std::string m_cacheErr;
    std::atomic<long long> m_cachedAt{0};
    std::atomic<bool> m_everOk{false};
    std::atomic<int> m_version{0};
    std::atomic<bool> m_refreshing{false};
};
