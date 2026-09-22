// ConfigSchema.h - 配置字段名、默认值、枚举取值（单一事实来源）
#pragma once
#include <string>

namespace cfg {

inline const char* kAppName = "TraeRelay";
inline const char* kDefaultHost = "127.0.0.1";
inline const int kDefaultPort = 8317;
inline const char* kApiKeyPrefix = "sk-trae-";

// 当前 Trae 模型目录出现的原生档位。
inline const char* kEfforts[] = { "light", "low", "high", "extra_high" };
inline bool isValidEffort(const std::string& v) {
    for (auto e : kEfforts)
        if (v == e) return true;
    return false;
}
inline const char* effortLabel(const std::string& v) { // 官方中文标签
    if (v == "light" || v == "low") return "轻";
    if (v == "high") return "高";
    if (v == "extra_high") return "极高";
    return "";
}
inline const char* effortFromLabel(const std::string& zh) {
    if (zh == "轻") return "low";
    if (zh == "高") return "high";
    if (zh == "极高") return "extra_high";
    return "";
}

} // namespace cfg
