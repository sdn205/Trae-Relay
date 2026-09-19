#include "window/WindowController.h"
#include "app/Service.h"
#include "ui/Dpi.h"
#include <dwmapi.h>

namespace ui {
using namespace visual;

WindowController::WindowController()
    : brCanvas_(CreateSolidBrush(C_CANVAS)), brSurface_(CreateSolidBrush(C_SURFACE)) {
    initializeDrawing();
}

WindowController::~WindowController() {
    startupWorker_.request_stop();
    creditsWorker_.request_stop();
    checkinWorker_.request_stop();
    if (startupWorker_.joinable()) startupWorker_.join();
    if (creditsWorker_.joinable()) creditsWorker_.join();
    if (checkinWorker_.joinable()) checkinWorker_.join();
    if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
    trayRemove();
    service::stop();
    deleteFonts();
    UnregisterClassW(L"TraeRelayPage", GetModuleHandleW(nullptr));
    UnregisterClassW(L"TraeRelayMain", GetModuleHandleW(nullptr));
    DeleteObject(brCanvas_);
    DeleteObject(brSurface_);
    releaseDrawing();
}

LRESULT CALLBACK WindowController::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<WindowController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<WindowController*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    auto result = self->handleMessage(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        releaseSurface(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
    }
    return result;
}

LRESULT CALLBACK WindowController::pageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<WindowController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<WindowController*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    LRESULT result = self ? self->handlePageMessage(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) releaseSurface(hwnd);
    return result;
}


HWND WindowController::pageHwnd(int page) {
    switch (page) {
    case PAGE_STATUS: return pageStatus_;
    case PAGE_SETTINGS: return pageSettings_;
    default: return pageUsage_;
    }
}

bool WindowController::isChecked(int id) {
    auto it = checkStates_.find(id);
    return it != checkStates_.end() && it->second;
}

void WindowController::setChecked(int id, bool value) { checkStates_[id] = value; }

void WindowController::toggleChecked(int id) { checkStates_[id] = !isChecked(id); }

void WindowController::rebuildFonts(HWND hwnd) {
    createFonts(dpi::forWindow(hwnd));
    applyControlFonts(hwnd);
}

void WindowController::configureTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    COLORREF caption = C_INK;
    COLORREF text = C_SURFACE;
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36, &text, sizeof(text));
}

void WindowController::registerClasses() {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW pageClass{};
    pageClass.cbSize = sizeof(pageClass);
    pageClass.lpfnWndProc = pageProc;
    pageClass.hInstance = instance;
    pageClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pageClass.hbrBackground = brCanvas_;
    pageClass.lpszClassName = L"TraeRelayPage";
    RegisterClassExW(&pageClass);
    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = wndProc;
    mainClass.hInstance = instance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = brCanvas_;
    mainClass.lpszClassName = L"TraeRelayMain";
    mainClass.hIcon = appIcon();
    mainClass.hIconSm = appIcon();
    RegisterClassExW(&mainClass);
}

int WindowController::runGui(bool startMinimized) {
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    registerClasses();
    // 初始窗口尺寸按主显示器有效 DPI（窗口未创建时 GetDpiForWindow 不可用、
    // GetDpiForSystem 在部分机器返回 96 与实际缩放不符）。
    UINT initialDpi = dpi::forPrimaryMonitor();
    int width = MulDiv(1040, initialDpi, 96);
    int height = MulDiv(740, initialDpi, 96);
    // 创建时直接定位到主屏工作区正中央，避免窗口先出现在默认位置再移动
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int winX = work.left + ((work.right - work.left) - width) / 2;
    int winY = work.top + ((work.bottom - work.top) - height) / 2;
    if (winX < work.left) winX = work.left;
    if (winY < work.top) winY = work.top;
    HWND hwnd = CreateWindowExW(0, L"TraeRelayMain", L"Trae Relay",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, winX, winY, width, height,
                                nullptr, nullptr, instance, this);
    trayAdd(hwnd);
    if (startMinimized) {
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

} // namespace ui
