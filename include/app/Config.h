// Config.h - config.json 读写 / 校验 / clamp / 热加载
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "common/Json.h"
#include "common/Settings.h"
#include "accounts/Storage.h"

struct Config {
    // ===== service =====
    std::string serviceHost = "127.0.0.1";
    int servicePort = 8317;
    bool allowLan = false;
    std::string apiKey;
    bool allowAnyApiKey = false;
    bool apiKeyProtected = false;
    int upstreamTimeoutSec = 120;
    int firstEventTimeoutSec = 120;
    int maxConcurrentPerAccount = 2;
    int minRequestIntervalMs = 0;
    // ===== startup =====
    bool autoStart = false;              // 仅回显，权威在注册表
    bool startMinimizedToTray = true;
    bool minimizeToTrayOnClose = true;
    std::string trayClickAction = "show";   // show | none
    bool singleInstance = true;
    // ===== upstream =====
    // 固定 solo 通道（/api/agent/v3/llm_utils_chat）。版本头默认留空，启动时
    // 从本机 Trae 安装的 product.json 动态探测，探测失败才用编译期兜底。
    std::string ideVersion;
    std::string ideVersionCode;
    // ===== defaults =====
    std::string defaultReasoningEffort;  // "" = null
    int defaultIsMaxMode = 0;
    long long defaultMaxContextWindow = 0;
    bool defaultStream = false;         // OpenAI 规范默认 false；客户端显式传 stream 照常生效
    bool autoContinue = true;
    int maxAutoContinue = 10;
    // ===== responses =====
    bool responsesEnabled = true;
    bool responsesMapReasoningSummary = true;
    int responsesSessionCacheSize = 64;
    bool responsesSessionCachePersist = false;
    // ===== accounts =====
    bool accountsAutoDiscover = true;
    bool checkinEnabled = true;
    int checkinHour = 10;
    int checkinMinute = 0;
    std::string poolSelectBy = "credits";   // credits | roundRobin
    // ===== logging =====
    std::string logLevel = "info";
    bool loggingEnabled = true;
    std::string logDir;                     // 空 = <exe目录>\logs
    int logRetainDays = 7;
    bool logRedactSecrets = true;
    // ===== models =====
    std::vector<ModelConfig> models;
    // ===== 运行时 =====
    std::string path;                       // 实际配置文件路径（exe 同目录 config.json）
    bool readOnlyMode = false;              // version 过高 → 拒绝写回
    int version = 1;
    Json rawUnknown;                        // 保留未知字段

    bool load(const std::string& explicitPath, std::string& err);
    bool save(std::string& err);            // tmp + 原子替换
    Json toJson() const;
    // 解析请求模型名 → 配置（含 alias），找不到返回 nullptr
    const ModelConfig* modelConfig(const std::string& name) const;
    void upsertModel(const ModelConfig& m);

    static Config& instance();
    std::mutex mtx;
private:
    void applyJson(const Json& j);
    // 配置未显式给出版本头时，从本机 Trae 安装动态探测并兜底。
    void resolveIdeVersion();
    static std::string resolvePath(const std::string& explicitPath);
};

// Config → core 设置快照：逐字段拷贝，core 只读这份不可变数据，不反向依赖 app 层。
// 启动与每次"应用设置"后调用 settings::set(makeCoreSettings(cfg)) 推送。
std::shared_ptr<const CoreSettings> makeCoreSettings(const Config& cfg);
