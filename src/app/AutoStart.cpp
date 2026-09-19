// AutoStart.cpp
#include "app/AutoStart.h"
#include <windows.h>

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kValueName = L"TraeRelay";

static std::wstring exePath() {
    wchar_t buf[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(L"\"") + buf + L"\" --tray";
}

bool autostart::isEnabled() {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExW(h, kValueName, nullptr, &type, nullptr, &size);
    bool ok = r == ERROR_SUCCESS && type == REG_SZ && size > 2;
    RegCloseKey(h);
    return ok;
}

bool autostart::setEnabled(bool enable, std::string& err) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &h) != ERROR_SUCCESS) {
        err = "打开注册表 Run 键失败（权限？）";
        return false;
    }
    LONG r;
    if (enable) {
        std::wstring val = exePath();
        DWORD bytes = (DWORD)((val.size() + 1) * sizeof(wchar_t));
        r = RegSetValueExW(h, kValueName, 0, REG_SZ, (const BYTE*)val.c_str(), bytes);
    } else {
        r = RegDeleteValueW(h, kValueName);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(h);
    if (r != ERROR_SUCCESS) {
        err = "注册表写入失败（错误码 " + std::to_string(r) + "）";
        return false;
    }
    return true;
}
