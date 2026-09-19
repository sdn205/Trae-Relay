// ReasoningEffort.h - R5 降级：System 前缀注入（按模型族映射）
#pragma once
#include <string>

namespace effort {

// 按模型 config_name + 档位（low/high/xhigh）返回注入串；无匹配返回 ""（静默 no-op）
std::string prefixFor(const std::string& configName, const std::string& effortValue);

// 用 <<think_effort>> 块包裹注入串
std::string wrap(const std::string& prefix);

// 剥离文本中已有的 <<think_effort>> 块（防重复注入）
std::string stripBlock(const std::string& text);

} // namespace effort
