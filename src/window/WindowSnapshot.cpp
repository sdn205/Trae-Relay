#include "window/WindowController.h"
#include "ui/Dpi.h"
#include "common/Log.h"

namespace ui {
using namespace visual;

const wchar_t* WindowController::pageName(int page) {
    switch (page) {
    case PAGE_STATUS: return L"status";
    case PAGE_SETTINGS: return L"settings";
    default: return L"usage";
    }
}


void WindowController::printChildControls(HWND page, HDC dc, int orgX, int orgY) {
    for (HWND child = GetWindow(page, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t cls[64]{};
        GetClassNameW(child, cls, 64);
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        if (wcscmp(cls, L"TraeRelayPage") == 0) continue; // 页面子窗由对应页的渲染负责
        RECT wr{};
        GetWindowRect(child, &wr);
        POINT pt{ wr.left, wr.top };
        ScreenToClient(page, &pt);
        POINT oldOrg{};
        SetViewportOrgEx(dc, orgX + pt.x, orgY + pt.y, &oldOrg);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
        SetViewportOrgEx(dc, oldOrg.x, oldOrg.y, nullptr);
    }
}

int WindowController::runSnapshot(const std::string& dirUtf8) {
    snapshotMode_ = true;
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    registerClasses();
    UINT dpiV = dpi::forPrimaryMonitor();
    int width = MulDiv(1040, dpiV, 96);
    int height = MulDiv(740, dpiV, 96);
    HWND hwnd = CreateWindowExW(0, L"TraeRelayMain", L"Trae Relay", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0,
                                width, height, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    hwnd_ = hwnd; // WM_CREATE 在快照模式下只记录句柄
    rebuildFonts(hwnd);
    createNavControls(hwnd);
    createPages(hwnd);
    layoutAll(hwnd);
    setPage(PAGE_STATUS);
    refreshStatusPage(hwnd);

    RECT client{};
    GetClientRect(hwnd, &client);
    width = client.right;
    height = client.bottom;
    std::wstring dir = toWide(dirUtf8);
    CreateDirectoryW(dir.c_str(), nullptr);
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    bool savedAll = true;
    int top = s(hwnd, TOPBAR_H);
    for (int page : { PAGE_STATUS, PAGE_USAGE, PAGE_SETTINGS }) {
        setPage(page);
        HWND ph = pageHwnd(page);
        if (page == PAGE_USAGE) loadUsagePage();
        RECT prc{};
        GetClientRect(ph, &prc);
        BITMAPINFO pbi = bi;
        pbi.bmiHeader.biWidth = prc.right;
        pbi.bmiHeader.biHeight = -prc.bottom;
        void* pbits = nullptr;
        HBITMAP pbmp = CreateDIBSection(screen, &pbi, DIB_RGB_COLORS, &pbits, nullptr, 0);
        HDC pdc = CreateCompatibleDC(screen);
        HGDIOBJ oldP = SelectObject(pdc, pbmp);
        renderPageContent(pdc, ph, page);
        {
            RECT chrome{0, 0, width, top};
            Canvas canvas(hwnd, mem, chrome, C_INK);
            renderChrome(canvas, hwnd);
        }
        BitBlt(mem, 0, top, prc.right, prc.bottom, pdc, 0, 0, SRCCOPY);
        printChildControls(ph, mem, 0, top);
        printChildControls(hwnd, mem, 0, 0); // 顶带 nav tab
        GdiFlush();
        std::wstring path = dir + L"\\page-" + pageName(page) + L".png";
        savedAll = savePng(bmp, path) && savedAll;
        SelectObject(pdc, oldP);
        DeleteObject(pbmp);
        DeleteDC(pdc);
    }
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DestroyWindow(hwnd);
    LOG_I("快照渲染完成: %s", dirUtf8.c_str());
    return savedAll ? 0 : 2;
}

} // namespace ui
