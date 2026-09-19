#include "window/WindowController.h"
#include "app/AutoStart.h"
#include "app/Config.h"
#include "app/Service.h"

namespace ui {
using namespace visual;

HICON WindowController::appIcon() {
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

void WindowController::trayAdd(HWND hwnd) {
    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_APP_TRAY;
    nid_.hIcon = appIcon();
    wcscpy_s(nid_.szTip, L"Trae Relay");
    Shell_NotifyIconW(NIM_ADD, &nid_);
    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    trayAdded_ = true;
}

void WindowController::trayRemove() {
    if (!trayAdded_) return;
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    trayAdded_ = false;
}

void WindowController::showMainWindow(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

void WindowController::showTrayMenu(HWND hwnd) {
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    auto& cfg = Config::instance();
    AppendMenuW(menu, MF_STRING, 1, L"显示主窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (autostart::isEnabled() ? MF_CHECKED : 0), 3, L"开机自启动");
    AppendMenuW(menu, MF_STRING | (cfg.minimizeToTrayOnClose ? MF_CHECKED : 0), 4,
                L"关闭时最小化到托盘");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 9, L"退出");
    SetForegroundWindow(hwnd);
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    switch (command) {
    case 1:
        showMainWindow(hwnd);
        break;
    case 3: {
        std::string error;
        bool enabled = !autostart::isEnabled();
        if (autostart::setEnabled(enabled, error)) {
            Config::instance().autoStart = enabled;
            std::string saveError;
            Config::instance().save(saveError);
        } else {
            MessageBoxA(hwnd, error.c_str(), "错误", MB_ICONERROR);
        }
        setChecked(IDC_CHK_AUTOSTART, autostart::isEnabled());
        InvalidateRect(GetDlgItem(pageSettings_, IDC_CHK_AUTOSTART), nullptr, FALSE);
        break;
    }
    case 4: {
        cfg.minimizeToTrayOnClose = !cfg.minimizeToTrayOnClose;
        std::string error;
        cfg.save(error);
        setChecked(IDC_CHK_MINCLOSE, cfg.minimizeToTrayOnClose);
        InvalidateRect(GetDlgItem(pageSettings_, IDC_CHK_MINCLOSE), nullptr, FALSE);
        break;
    }
    case 9:
        trayRemove();
        service::stop();
        PostQuitMessage(0);
        break;
    }
}

} // namespace ui
