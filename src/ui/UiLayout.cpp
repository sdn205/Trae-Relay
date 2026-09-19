#include "ui/UiLayout.h"
#include <algorithm>
namespace ui::visual {
R editCtrl(R box) { return { box.x + FieldMetrics::editInsetX, box.y + FieldMetrics::editInsetY,
    box.w - 2 * FieldMetrics::editInsetX, FieldMetrics::editHeight }; }
R comboCtrl(R box, int dropH) { return { box.x, box.y, box.w, dropH }; }
int usageVisibleRows(int height) {
    return std::max(1, (height - 104 - kUsageTop) / kUsageRowH);
}
UsageScrollbar usageScrollbar(int width, int height, int rowCount, int scroll) {
    UsageScrollbar bar;
    const int visible = usageVisibleRows(height);
    bar.maxScroll = std::max(0, rowCount - visible);
    if (!bar.maxScroll) return bar;
    // 放在表格右侧留白，不挤压现有列宽；轨道只覆盖数据区。
    bar.track = {width - 28, kUsageTop, 8, std::max(0, height - 104 - kUsageTop)};
    const int thumbHeight = std::min(bar.track.h, std::max(28, MulDiv(bar.track.h, visible, rowCount)));
    const int offset = MulDiv(bar.track.h - thumbHeight, std::clamp(scroll, 0, bar.maxScroll), bar.maxScroll);
    bar.thumb = {bar.track.x, bar.track.y + offset, bar.track.w, thumbHeight};
    return bar;
}
int pagerY(int height) { return height - 86; }

UsageCols usageColumns(int width) {
    static const double frac[5] = { 0.20, 0.22, 0.19, 0.23, 0.16 };
    UsageCols c{};
    double acc = 40;
    for (int i = 0; i < 5; ++i) {
        c.x[i] = (int)acc;
        c.w[i] = (int)(frac[i] * (width - 80));
        acc += c.w[i];
    }
    return c;
}
R accountCard(int width) { return { 40, 76, width - 80, 148 }; }
R endpointCard(int width) { return { 40, 240, width - 80, 144 }; }
R modelSettingsCard(int width) { return { 40, 400, width - 80, 164 }; }
R baseUrlBox(int width) { return { 108, 285, width - 212, FieldMetrics::height }; }
R apiKeyBox(int width) { return { 108, 331, width - 344, FieldMetrics::height }; }
R modelComboBox(int width) { return { 60, 447, width - 120, FieldMetrics::height }; }
R portBox(int) { return { 240, 129, 90, FieldMetrics::height }; }
R hourBox(int) { return { 440, 405, 52, FieldMetrics::height }; }
R minuteBox(int) { return { 528, 405, 52, FieldMetrics::height }; }
R logLevelBox(int) { return { 240, 501, 180, FieldMetrics::height }; }
}
