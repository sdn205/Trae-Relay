#pragma once
#include "ui/UiDrawing.h"
#include <vector>
#include "accounts/AccountPool.h"

namespace ui::visual {
struct UsageTableView {
    const std::vector<UsageRecord>& rows;
    int scroll, pageIndex, totalPages, pagerX;
    bool empty;
    bool dragging = false;
};
void paintUsageTable(Canvas& dc, int width, int height, const UsageTableView& view);
} // namespace ui::visual
