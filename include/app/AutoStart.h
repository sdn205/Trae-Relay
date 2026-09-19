// AutoStart.h - R3：开机自启动（HKCU Run 键）
#pragma once
#include <string>

namespace autostart {
// 读注册表实际状态（不缓存）
bool isEnabled();
// 写/删注册表；写入值为 "exe路径" --tray
bool setEnabled(bool enable, std::string& err);
} // namespace autostart
