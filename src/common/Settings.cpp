// Settings.cpp - 设置快照持有：互斥锁保护 shared_ptr 换份，读端无竞争
#include "common/Settings.h"
#include <cctype>
#include <mutex>

namespace {
std::mutex g_mtx;
std::shared_ptr<const CoreSettings> g_snap = std::make_shared<const CoreSettings>();
} // namespace

namespace settings {

std::shared_ptr<const CoreSettings> get() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_snap;
}

void set(std::shared_ptr<const CoreSettings> s) {
    if (!s) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_snap = std::move(s);
}

} // namespace settings

// 查找逻辑与原 Config::modelConfig 一致：小写化后按 name → alias 匹配
const ModelConfig* CoreSettings::modelConfig(const std::string& name) const {
    std::string ln = name;
    for (auto& c : ln) c = (char)tolower((unsigned char)c);
    for (auto& m : models) {
        std::string mn = m.name;
        for (auto& c : mn) c = (char)tolower((unsigned char)c);
        if (mn == ln) return &m;
        for (auto& a : m.alias) {
            std::string la = a;
            for (auto& c : la) c = (char)tolower((unsigned char)c);
            if (la == ln) return &m;
        }
    }
    return nullptr;
}
