#include "window/WindowController.h"
#include "ui/UiUsageTable.h"

namespace ui {
using namespace visual;

void WindowController::renderPageContent(HDC target, HWND hwnd, int page) {
    RECT client{};
    GetClientRect(hwnd, &client);
    Canvas dc(hwnd, target, client, C_CANVAS);
    int width = logicalWidth(hwnd);
    int height = logicalHeight(hwnd);
    if (page == PAGE_STATUS) paintStatusPage(dc, width, height);
    else if (page == PAGE_USAGE) {
        paintUsageTable(dc, width, height, { usageRows_, usageScroll_,
            usagePageIdx_, usageTotalPages_, pagerX_, usageEmpty_, usageDragging_ });
    }
    else paintSettingsPage(dc, width, height);
}

void WindowController::paintPage(HWND hwnd, int page) {
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(hwnd, &ps);
    renderPageContent(target, hwnd, page);
    EndPaint(hwnd, &ps);
}

void WindowController::layoutAll(HWND hwnd) {
    int width = logicalWidth(hwnd);
    int height = logicalHeight(hwnd);
    // 顶部仪器带上的横向 tab
    place(hwnd, navStatus_, { 20, 0, 84, TOPBAR_H });
    place(hwnd, navUsage_, { 108, 0, 84, TOPBAR_H });
    place(hwnd, navSettings_, { 196, 0, 92, TOPBAR_H });
    place(hwnd, pageStatus_, { 0, TOPBAR_H, width, height - TOPBAR_H });
    place(hwnd, pageSettings_, { 0, TOPBAR_H, width, height - TOPBAR_H });
    place(hwnd, pageUsage_, { 0, TOPBAR_H, width, height - TOPBAR_H });
    layoutStatusPage();
    layoutSettingsPage();
    layoutUsagePage();
}

void WindowController::createNavControls(HWND hwnd) {
    navStatus_ = makeControl(hwnd, L"运行总览", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                              IDC_NAV_STATUS);
    navUsage_ = makeControl(hwnd, L"使用记录", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                             IDC_NAV_USAGE);
    navSettings_ = makeControl(hwnd, L"偏好设置", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                                IDC_NAV_SETTINGS);
}

void WindowController::createPages(HWND hwnd) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    pageStatus_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"TraeRelayPage", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                   0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kPageControlBase + PAGE_STATUS), instance, this);
    pageSettings_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"TraeRelayPage", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                     0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kPageControlBase + PAGE_SETTINGS), instance, this);
    pageUsage_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"TraeRelayPage", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                  0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kPageControlBase + PAGE_USAGE), instance, this);
    createStatusControls();
    createSettingsControls();
    createUsageControls();
    applyControlFonts(hwnd);
}

void WindowController::setPage(int page) {
    if (activePage_ == page && (GetWindowLongPtrW(pageHwnd(page), GWL_STYLE) & WS_VISIBLE))
        return;
    activePage_ = page;
    ShowWindow(pageStatus_, page == PAGE_STATUS ? SW_SHOW : SW_HIDE);
    ShowWindow(pageSettings_, page == PAGE_SETTINGS ? SW_SHOW : SW_HIDE);
    ShowWindow(pageUsage_, page == PAGE_USAGE ? SW_SHOW : SW_HIDE);
    InvalidateRect(navStatus_, nullptr, FALSE);
    InvalidateRect(navUsage_, nullptr, FALSE);
    InvalidateRect(navSettings_, nullptr, FALSE);
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (page == PAGE_STATUS) {
        refreshStatusPage(hwnd_);
        refreshModelSettings(hwnd_);
        loadModelSelection(hwnd_);
    }
    if (page == PAGE_USAGE) loadUsagePage(); // 进页即拉当前分页
}

} // namespace ui

