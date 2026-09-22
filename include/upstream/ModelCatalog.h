// ModelCatalog.h - 模型配置表缓存（get_detail_param 主目录，stale-while-revalidate）
#pragma once
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include "common/Json.h"
#include "upstream/Upstream.h"

class Account;

class ModelCatalog {
public:
    // 返回最近的目录；首次启动只等待第一轮刷新，失败后按计划重试。
    std::vector<ModelCaps> all(Account& acc, std::string& err);
    // 单模型解析（all 的薄封装）
    bool get(Account& acc, const std::string& configName, ModelCaps& out, std::string& err);

    // ---- 纯内存访问（UI 线程用，永不发网络请求）----
    bool ready() const { return m_everOk.load(); }
    int version() const { return m_version.load(); }
    std::vector<ModelCaps> cached() const;
    // 启动唯一刷新线程：立即拉取，成功后 1 小时、失败后 5 分钟重试。
    void start();
    void stop();

    static ModelCatalog& instance();

private:
    // 一次完整拉取 + 合并 + 提交；成功更新缓存并自增 version
    bool refreshOnce(Account& acc, std::string& err);

    mutable std::mutex m_mtx;
    std::vector<ModelCaps> m_cache;
    std::string m_cacheErr;
    std::atomic<bool> m_everOk{false};
    std::atomic<int> m_version{0};
    bool m_attempted = false;
    std::condition_variable_any m_wake;
    std::jthread m_worker;
};
