// Settings.h - core 层运行设置快照：app 层（Config/MainWindow）负责从 Config
// 映射并推送，core 只读不可变快照，不感知 app/Config，依赖保持 app → core 单向。
#pragma once
#include <memory>
#include <string>
#include <vector>

// 模型覆盖项（config.json models 节点；原定义于 app/Config.h，core 侧同样需要读取）
struct ModelConfig {
    std::string name;                    // 官方 config_name（键）
    bool enabled = true;
    std::string displayName;             // 空 = 用原名
    std::vector<std::string> alias;
    std::string reasoningEffort;         // "" = 跟随 defaults
    int isMaxMode = 0;
    long long maxContextWindow = 0;      // 0 = null（取 max 档最大值）
    std::string extraSystemPrefix;       // "" = 无
    bool present = false;                // models 节点里是否出现
};

// core 消费的设置快照（字段默认值与 Config 保持一致；由 app 层 makeCoreSettings 填充）
struct CoreSettings {
    // ===== upstream =====
    std::string ideVersion;
    std::string ideVersionCode;
    int upstreamTimeoutSec = 120;
    int firstEventTimeoutSec = 120;
    // ===== service =====
    std::string serviceHost = "127.0.0.1";
    int servicePort = 8317;
    bool allowLan = false;
    std::string apiKey;
    bool allowAnyApiKey = false;
    // ===== defaults =====
    std::string defaultReasoningEffort;  // "" = null
    int defaultIsMaxMode = 0;
    long long defaultMaxContextWindow = 0;
    bool defaultStream = false;
    // ===== accounts =====
    int maxConcurrentPerAccount = 2;
    int minRequestIntervalMs = 0;
    std::string poolSelectBy = "credits";   // credits | roundRobin
    int poolCooldownSec = 60;
    int poolDisableAfterFails = 5;
    // ===== responses =====
    bool responsesEnabled = true;
    bool responsesMapReasoningSummary = true;
    int responsesSessionCacheSize = 64;
    bool responsesSessionCachePersist = false;
    // ===== models =====
    std::vector<ModelConfig> models;

    // 解析请求模型名 → 覆盖项（含 alias，大小写不敏感），找不到返回 nullptr
    const ModelConfig* modelConfig(const std::string& name) const;
};

namespace settings {
// 读：返回当前快照。请求入口取一次并持有 shared_ptr，整个请求期间视图一致，
// UI 线程中途换份也不会看到半新半旧的字段。
std::shared_ptr<const CoreSettings> get();
// 写：app 层在启动/应用设置时整份替换（原子），传空指针忽略。
void set(std::shared_ptr<const CoreSettings> s);
} // namespace settings
