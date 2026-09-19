#include "window/WindowController.h"
#include "ui/Dpi.h"
#include <algorithm>

namespace ui {
using namespace visual;

void WindowController::usagePagerItems(int cur, int total, int* slots) {
    std::fill_n(slots, 7, 0);
    if (total <= 0) return;
    cur = std::clamp(cur, 1, total);
    if (total <= 7) {
        for (int i = 0; i < total; ++i) slots[i] = i + 1;
    } else if (cur <= 4) {
        for (int i = 0; i < 5; ++i) slots[i] = i + 1;
        slots[5] = -1; slots[6] = total;
    } else if (cur >= total - 3) {
        slots[0] = 1; slots[1] = -1;
        for (int i = 2; i < 7; ++i) slots[i] = total - 6 + i;
    } else {
        slots[0] = 1; slots[1] = -1;
        slots[2] = cur - 1; slots[3] = cur; slots[4] = cur + 1;
        slots[5] = -1; slots[6] = total;
    }
}

void WindowController::layoutUsagePager(const UsagePagerLayout& layout) {
    const int width = layout.width, height = layout.height;
    // 分页条：‹ [槽×7] ›，条目数决定槽位横向排布
    int slots[7];
    usagePagerItems(layout.page + 1, layout.pages, slots);
    int items = 0;
    for (int i = 0; i < 7; ++i)
        if (slots[i] != 0) items++;
    int arrows = 2, btnW = 40, gap = 6;
    int count = items + arrows;
    int x = width - 40 - (count * btnW + (count - 1) * gap); // 右下角：右缘贴面板边距
    int y = pagerY(height);
    pagerX_ = x;
    auto placeBtn = [&](int id, const wchar_t* text, int px, bool enable) {
        HWND b = GetDlgItem(layout.window, id);
        if (!b) return;
        setControlText(b, text);
        place(layout.window, b, { px, y, btnW, 34 });
        enableControl(b, enable);
        ShowWindow(b, layout.empty ? SW_HIDE : SW_SHOW);
        InvalidateRect(b, nullptr, FALSE); // 页码文字不变时，当前页高亮仍可能改变。
    };
    placeBtn(IDC_BTN_PAGE_PREV, L"上一页", x, layout.page > 0);
    x += btnW + gap;
    for (int i = 0; i < 7; ++i) {
        int id = IDC_PAGER_SLOT0 + i;
        pagerSlotPage_[i] = slots[i];
        if (slots[i] == 0) { ShowWindow(GetDlgItem(layout.window, id), SW_HIDE); continue; }
        wchar_t label[16]{};
        if (slots[i] == -1) swprintf(label, 16, L"…");
        else swprintf(label, 16, L"%d", slots[i]);
        placeBtn(id, label, x, slots[i] != -1);
        x += btnW + gap;
    }
    placeBtn(IDC_BTN_PAGE_NEXT, L"下一页", x, layout.hasMore);
    InvalidateRect(layout.window, nullptr, FALSE);
}

void WindowController::loadUsagePage(bool forcePaint) {
    if (!pageUsage_) return;
    int width = logicalWidth(pageUsage_);
    int height = logicalHeight(pageUsage_);
    bool hasMore = false;
    int total = AccountPool::instance().usageTotalCount();
    int totalPages = std::max(1, (total + kUsagePageSize - 1) / kUsagePageSize);
    usagePageIdx_ = std::clamp(usagePageIdx_, 0, totalPages - 1);
    auto rows = AccountPool::instance().usagePage(usagePageIdx_, kUsagePageSize, hasMore);
    // 定时刷新只比较实际显示的数据，未变化时不触碰窗口或无效区。
    bool rowsChanged = rows.size() != usageRows_.size() ||
        !std::equal(rows.begin(), rows.end(), usageRows_.begin(),
            [](const UsageRecord& a, const UsageRecord& b) {
                return a.ts == b.ts && a.model == b.model && a.endpoint == b.endpoint &&
                    a.in == b.in && a.out == b.out && a.cache == b.cache &&
                    a.creditsKnown == b.creditsKnown && a.creditsDelta == b.creditsDelta &&
                    a.merged == b.merged;
            });
    UsagePagerLayout currentPager{ pageUsage_, usagePageIdx_, totalPages, width, height,
        dpi::forWindow(pageUsage_), total == 0, hasMore };
    bool pagerChanged = currentPager != previousPager_;
    if (!forcePaint && !rowsChanged && !pagerChanged) return;
    usageRows_ = std::move(rows);
    usageHasMore_ = hasMore;
    usageTotalPages_ = totalPages;
    usageEmpty_ = total == 0;
    const int maxScroll = std::max(0, static_cast<int>(usageRows_.size()) - usageVisibleRows(height));
    usageScroll_ = std::clamp(usageScroll_, 0, maxScroll);
    if (usageDragging_ && maxScroll == 0) SendMessageW(pageUsage_, WM_CANCELMODE, 0, 0);
    if (!pagerChanged) {
        RECT table{ 0, 0, s(pageUsage_, width), s(pageUsage_, pagerY(height)) };
        InvalidateRect(pageUsage_, forcePaint ? nullptr : &table, FALSE);
        return;
    }
    previousPager_ = currentPager;
    layoutUsagePager(currentPager);
}

void WindowController::layoutUsagePage() {
    loadUsagePage();
}

void WindowController::scrollUsageTo(int position) {
    const int maxScroll = std::max(0, static_cast<int>(usageRows_.size()) - usageVisibleRows(logicalHeight(pageUsage_)));
    const int next = std::clamp(position, 0, maxScroll);
    if (next == usageScroll_) return;
    usageScroll_ = next;
    InvalidateRect(pageUsage_, nullptr, FALSE);
}

void WindowController::createUsageControls() {
    makeControl(pageUsage_, L"", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                IDC_BTN_PAGE_PREV);
    makeControl(pageUsage_, L"", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                IDC_BTN_PAGE_NEXT);
    for (int i = 0; i < 7; ++i)
        makeControl(pageUsage_, L"", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                    IDC_PAGER_SLOT0 + i);
}

} // namespace ui
