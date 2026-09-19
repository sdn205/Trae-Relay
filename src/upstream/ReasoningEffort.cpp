// ReasoningEffort.cpp - 前缀注入串（来自 Ttungx/trae-solo-local-api 实测映射）
#include "upstream/ReasoningEffort.h"

static const char* PREFIX_HIGH = "Reasoning Effort: High\n\n";

static const char* PREFIX_MAX_SHORT = "Reasoning Effort: Max\n\n";

static const char* PREFIX_KIMI_LOW =
    "<critical_constraints>\n"
    "Reasoning Mode: Token-efficient and concise.\n"
    "Rush through reasoning, be as concise as possible. Full send; never draft.\n"
    "Do NOT use progressive refinement or iterative self-criticism loops.\n"
    "You are allowed a maximum of one draft before outputting the final response.\n"
    "BAN all moralizing, conjecture, and assumption about actions or motives. Stick to the facts.\n"
    "</critical_constraints>\n\n";

static const char* PREFIX_MAX_ABS =
    "Reasoning Effort: Absolute maximum with no shortcuts permitted. You MUST be very thorough in "
    "your thinking and comprehensively decompose the problem to resolve the root cause, rigorously "
    "stress-testing your logic against all potential paths, edge cases, and adversarial scenarios. "
    "Explicitly write out your entire deliberation process, documenting every intermediate step, "
    "considered alternative, and rejected hypothesis to ensure absolutely no assumption is left "
    "unchecked.\n\n";

static std::string lowerCopy(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

namespace effort {

std::string prefixFor(const std::string& configName, const std::string& effortValue) {
    std::string m = lowerCopy(configName);
    std::string e = lowerCopy(effortValue);
    bool isMax = e == "xhigh" || e == "max" || e == "extra_high" || e == "ultra";
    if (m == "glm-5.2") {
        if (e == "high") return PREFIX_HIGH;
        if (isMax) return PREFIX_MAX_SHORT; // glm-5.2 禁用 Absolute 长串
        return "";
    }
    if (m == "deepseek-v4-pro") {
        if (isMax) return PREFIX_MAX_ABS; // DeepSeek-V4-Pro 短 Max 无效，必须长串
        return "";
    }
    if (m == "kimi-k2.7-code") {
        if (e == "low") return PREFIX_KIMI_LOW;
        if (isMax) return PREFIX_MAX_ABS;
        return "";
    }
    // 通用 fallback：solo 通道没有原生档位字段，未实测的模型族至少通过
    // system 前缀表达努力程度（模型不识别时仅为普通指令，无害）。
    if (isMax) return PREFIX_MAX_SHORT;
    if (e == "high") return PREFIX_HIGH;
    return "";
}

std::string wrap(const std::string& prefix) {
    if (prefix.empty()) return "";
    // 尾部多 \n 归一为一个 \n
    std::string p = prefix;
    while (p.size() >= 2 && p[p.size() - 1] == '\n' && p[p.size() - 2] == '\n') p.pop_back();
    return "<<think_effort>>\n" + p + "\n<</think_effort>>\n\n";
}

std::string stripBlock(const std::string& text) {
    static const char* kOpen = "<<think_effort>>";
    static const char* kClose = "<</think_effort>>";
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t start = text.find(kOpen, pos);
        if (start == std::string::npos) {
            out += text.substr(pos);
            break;
        }
        out += text.substr(pos, start - pos);
        size_t end = text.find(kClose, start);
        if (end == std::string::npos) break; // 无闭合：其余丢弃
        pos = end + strlen(kClose);
        while (pos < text.size() && text[pos] == '\n') ++pos;
    }
    return out;
}

} // namespace effort
