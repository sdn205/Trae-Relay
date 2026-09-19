// ConfigSchema.h - 配置字段名、默认值、枚举取值（单一事实来源）
#pragma once
#include <string>

namespace cfg {

inline const char* kAppName = "TraeRelay";
inline const char* kDefaultHost = "127.0.0.1";
inline const int kDefaultPort = 8317;
inline const char* kApiKeyPrefix = "sk-trae-";

// reasoning_effort 合法档位（线上值；extra_high 为 chat_v3 目录的极高别名）
inline const char* kEfforts[] = { "low", "high", "xhigh", "extra_high" };
inline bool isValidEffort(const std::string& v) {
    for (auto e : kEfforts)
        if (v == e) return true;
    return false;
}
inline const char* effortLabel(const std::string& v) { // 官方中文标签
    if (v == "low") return "轻";
    if (v == "high") return "高";
    if (v == "xhigh" || v == "extra_high") return "极高";
    return "";
}
inline const char* effortFromLabel(const std::string& zh) {
    if (zh == "轻") return "low";
    if (zh == "高") return "high";
    if (zh == "极高") return "xhigh";
    return "";
}

} // namespace cfg
