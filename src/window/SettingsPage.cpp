#include "window/WindowController.h"
#include "app/AutoStart.h"
#include "app/Config.h"
#include "app/Service.h"
#include "common/Crypto.h"
#include "common/Log.h"
#include <algorithm>
#include <windowsx.h>

namespace ui {
using namespace visual;

void WindowController::paintSettingsPage(Canvas& dc, int width, int) {
    paintPageHeader(dc, width, L"偏好设置");
    paintPanel(dc, 40, 76, width - 80, 476);
    // —— 服务 ——
    paintGroupLabel(dc, 60, 96, L"服务");
    drawText(dc, L"监听端口", { 60, 128, 180, 32 }, FBody, C_TEXT,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    paintField(dc, pageSettings_, IDC_ED_PORT);
    drawDivider(dc, 60, 176, width - 60);
    // —— 启动与托盘（托盘单击固定为唤出主窗口，不再提供选项）——
    paintGroupLabel(dc, 60, 192, L"启动与托盘");
    drawDivider(dc, 60, 356, width - 60);
    // —— 自动化 ——
    paintGroupLabel(dc, 60, 372, L"自动化");
    drawText(dc, L"签到时间", { 360, 403, 72, 32 }, FBody, C_TEXT,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    paintField(dc, pageSettings_, IDC_ED_HOUR);
    drawText(dc, L"时", { 500, 403, 24, 32 }, FSmall, C_MUTED,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    paintField(dc, pageSettings_, IDC_ED_MINUTE);
    drawText(dc, L"分", { 588, 403, 24, 32 }, FSmall, C_MUTED,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    drawDivider(dc, 60, 452, width - 60);
    // —— 日志 ——
    paintGroupLabel(dc, 60, 468, L"日志");
    drawText(dc, L"日志级别", { 60, 500, 180, 32 }, FBody, C_TEXT,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    paintField(dc, pageSettings_, IDC_CB_LOGLEVEL);
}

void WindowController::layoutSettingsPage() {
    int width = logicalWidth(pageSettings_);
    placeField(pageSettings_, IDC_ED_PORT);
    place(pageSettings_, GetDlgItem(pageSettings_, IDC_CHK_LAN), { 360, 129, std::max(200, width - 420), 30 });
    place(pageSettings_, GetDlgItem(pageSettings_, IDC_CHK_AUTOSTART), { 60, 224, 220, 30 });
    place(pageSettings_, GetDlgItem(pageSettings_, IDC_CHK_STARTMIN), { 60, 266, 240, 30 });
    place(pageSettings_, GetDlgItem(pageSettings_, IDC_CHK_MINCLOSE), { 60, 308, 260, 30 });
    place(pageSettings_, GetDlgItem(pageSettings_, IDC_CHK_CHECKIN), { 60, 403, 240, 32 });
    placeField(pageSettings_, IDC_ED_HOUR);
    placeField(pageSettings_, IDC_ED_MINUTE);
    placeField(pageSettings_, IDC_CB_LOGLEVEL);
}

void WindowController::createSettingsControls() {
    auto& cfg = Config::instance();
    makeControl(pageSettings_, L"", WS_VISIBLE | ES_NUMBER, IDC_ED_PORT);
    makeControl(pageSettings_, L"允许局域网访问",
                WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_CHK_LAN);
    makeControl(pageSettings_, L"开机自启动", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                IDC_CHK_AUTOSTART);
    makeControl(pageSettings_, L"启动时最小化到托盘", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                IDC_CHK_STARTMIN);
    makeControl(pageSettings_, L"关闭窗口时最小化到托盘", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                IDC_CHK_MINCLOSE);
    makeControl(pageSettings_, L"每日自动签到", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                IDC_CHK_CHECKIN);
    makeControl(pageSettings_, L"", WS_VISIBLE | ES_NUMBER, IDC_ED_HOUR);
    makeControl(pageSettings_, L"", WS_VISIBLE | ES_NUMBER, IDC_ED_MINUTE);
    makeControl(pageSettings_, L"",
                WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                IDC_CB_LOGLEVEL);
    for (const wchar_t* level : { L"trace", L"debug", L"info", L"warn", L"error" })
        ComboBox_AddString(GetDlgItem(pageSettings_, IDC_CB_LOGLEVEL), level);
    wchar_t value[64]{};
    swprintf(value, 64, L"%d", cfg.servicePort);
    setControlText(GetDlgItem(pageSettings_, IDC_ED_PORT), value);
    setChecked(IDC_CHK_LAN, cfg.allowLan);
    setChecked(IDC_CHK_AUTOSTART, autostart::isEnabled());
    setChecked(IDC_CHK_STARTMIN, cfg.startMinimizedToTray);
    setChecked(IDC_CHK_MINCLOSE, cfg.minimizeToTrayOnClose);
    setChecked(IDC_CHK_CHECKIN, cfg.checkinEnabled);
    swprintf(value, 64, L"%02d", cfg.checkinHour);
    setControlText(GetDlgItem(pageSettings_, IDC_ED_HOUR), value);
    swprintf(value, 64, L"%02d", cfg.checkinMinute);
    setControlText(GetDlgItem(pageSettings_, IDC_ED_MINUTE), value);
    int level = cfg.logLevel == "trace" ? 0 : cfg.logLevel == "debug" ? 1
                  : cfg.logLevel == "warn" ? 3 : cfg.logLevel == "error" ? 4 : 2;
    ComboBox_SetCurSel(GetDlgItem(pageSettings_, IDC_CB_LOGLEVEL), level);
}

void WindowController::applySettingsInstant(HWND hwnd) {
    if (!pageSettings_ || applyingSettings_) return;
    applyingSettings_ = true;
    struct SavingScope { bool& active; ~SavingScope() { active = false; } } saving{applyingSettings_};
    auto& cfg = Config::instance();
    wchar_t value[64]{};
    GetWindowTextW(GetDlgItem(pageSettings_, IDC_ED_PORT), value, 64);
    int port = _wtoi(value);
    if (port <= 0 || port > 65535) {
        MessageBoxW(hwnd, L"端口必须在 1 到 65535 之间。", L"设置无效", MB_ICONERROR);
        swprintf(value, 64, L"%d", cfg.servicePort);
        setControlText(GetDlgItem(pageSettings_, IDC_ED_PORT), value);
        return;
    }
    GetWindowTextW(GetDlgItem(pageSettings_, IDC_ED_HOUR), value, 64);
    int hour = _wtoi(value);
    GetWindowTextW(GetDlgItem(pageSettings_, IDC_ED_MINUTE), value, 64);
    int minute = _wtoi(value);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        MessageBoxW(hwnd, L"签到时间必须是 00:00 到 23:59。", L"设置无效", MB_ICONERROR);
        swprintf(value, 64, L"%02d", cfg.checkinHour);
        setControlText(GetDlgItem(pageSettings_, IDC_ED_HOUR), value);
        swprintf(value, 64, L"%02d", cfg.checkinMinute);
        setControlText(GetDlgItem(pageSettings_, IDC_ED_MINUTE), value);
        return;
    }
    bool listenerChanged = port != cfg.servicePort || isChecked(IDC_CHK_LAN) != cfg.allowLan;
    cfg.servicePort = port;
    cfg.allowLan = isChecked(IDC_CHK_LAN);
    if (cfg.allowLan && cfg.apiKey.empty()) cfg.apiKey = crypto::genApiKey();
    cfg.startMinimizedToTray = isChecked(IDC_CHK_STARTMIN);
    cfg.minimizeToTrayOnClose = isChecked(IDC_CHK_MINCLOSE);
    cfg.checkinEnabled = isChecked(IDC_CHK_CHECKIN);
    cfg.checkinHour = hour;
    cfg.checkinMinute = minute;
    int level = ComboBox_GetCurSel(GetDlgItem(pageSettings_, IDC_CB_LOGLEVEL));
    cfg.logLevel = level == 0 ? "trace" : level == 1 ? "debug" : level == 3 ? "warn"
                                                          : level == 4 ? "error" : "info";
    bool wantAutostart = isChecked(IDC_CHK_AUTOSTART);
    std::string error;
    if (wantAutostart != autostart::isEnabled()) {
        if (!autostart::setEnabled(wantAutostart, error))
            MessageBoxA(hwnd, error.c_str(), "开机自启设置失败", MB_ICONERROR);
    }
    cfg.autoStart = autostart::isEnabled();
    if (!cfg.save(error)) {
        MessageBoxA(hwnd, error.c_str(), "保存失败", MB_ICONERROR);
        return;
    }
    // 配置落盘后同步更新当前进程的日志过滤器，使日志级别立即生效。
    LogLevel runtimeLogLevel = cfg.logLevel == "trace" ? LogLevel::Trace
                             : cfg.logLevel == "debug" ? LogLevel::Debug
                             : cfg.logLevel == "warn" ? LogLevel::Warn
                             : cfg.logLevel == "error" ? LogLevel::Error
                             : LogLevel::Info;
    logSetLevel(runtimeLogLevel);
    // 配置已变：重推 core 设置快照（端口变化随后重启服务，其余字段热生效）
    settings::set(makeCoreSettings(cfg));
    if (listenerChanged && service::running()) {
        service::stop();
        if (!service::start(error)) MessageBoxA(hwnd, error.c_str(), "重启服务失败", MB_ICONERROR);
    }
    setControlText(GetDlgItem(pageStatus_, IDC_ED_APIKEY), toWide(cfg.apiKey).c_str());
    refreshStatusPage(hwnd);
    // 端口/endpoint 变化后顶带与状态页同步
    InvalidateRect(hwnd, nullptr, FALSE);
    InvalidateRect(pageStatus_, nullptr, FALSE);
}

} // namespace ui
