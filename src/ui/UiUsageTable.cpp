#include "ui/UiUsageTable.h"
#include "ui/UiDrawing.h"
#include "ui/UiLayout.h"
#include <cmath>
#include <ctime>

namespace ui::visual {
// 千分位（token 数展示对齐 sub2 风格）
static std::wstring usageComma(long long v) {
    wchar_t raw[32]{};
    swprintf(raw, 32, L"%lld", v);
    std::wstring in = raw, out;
    int n = (int)in.size();
    for (int i = 0; i < n; ++i) {
        out.push_back(in[i]);
        int left = n - 1 - i;
        if (left > 0 && left % 3 == 0 && in[i] != '-') out.push_back(',');
    }
    return out;
}

// sub2api formatCacheTokens：≥1M→"X.XM"，≥1K→"X.XK"，其余千分位
static std::wstring usageCacheTokens(long long v) {
    wchar_t buf[32]{};
    if (v >= 1000000) {
        swprintf(buf, 32, L"%.1fM", v / 1000000.0);
        return buf;
    }
    if (v >= 1000) {
        swprintf(buf, 32, L"%.1fK", v / 1000.0);
        return buf;
    }
    return usageComma(v);
}

// TOKEN icons share Canvas geometry and rounded stroke rendering.
static void drawTokArrow(Canvas& canvas, int x, int y, int box, bool down, COLORREF color) {
    float k = box / 24.f;
    auto p = [&](float u, float v) { return Point{x + u * k, y + v * k}; };
    Point head[]{p(down ? 19.f : 5.f, down ? 14.f : 10.f), p(12, down ? 21.f : 3.f),
        p(down ? 5.f : 19.f, down ? 14.f : 10.f)};
    canvas.polyline(head, 3, color, 2 * k);
    canvas.line(p(12, 3), p(12, 21), color, 2 * k);
}
static void drawTokBox(Canvas& canvas, int x, int y, int box, COLORREF color) {
    float k = box / 24.f;
    auto point = [&](float u, float v) { return D2D1::Point2F(x + u * k, y + v * k); };
    ComPtr<ID2D1Factory> factory;
    canvas.target()->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    const float q = 2.f * .552284749f;
    auto begin = [&](float u, float v) { sink->BeginFigure(point(u, v), D2D1_FIGURE_BEGIN_HOLLOW); };
    auto line = [&](float u, float v) { sink->AddLine(point(u, v)); };
    auto curve = [&](float a, float b, float c, float d, float e, float f) {
        sink->AddBezier(D2D1::BezierSegment(point(a, b), point(c, d), point(e, f)));
    };
    auto end = [&] { sink->EndFigure(D2D1_FIGURE_END_OPEN); };
    begin(5, 8); line(19, 8); end();
    begin(5, 8); curve(5-q, 8, 3, 6+q, 3, 6); curve(3, 6-q, 5-q, 4, 5, 4);
    line(19, 4); curve(19+q, 4, 21, 6-q, 21, 6); curve(21, 6+q, 19+q, 8, 19, 8); end();
    begin(5, 8); line(5, 18); curve(5, 18+q, 7-q, 20, 7, 20); line(17, 20);
    curve(17+q, 20, 19, 18+q, 19, 18); line(19, 8); end();
    begin(10, 12); line(14, 12); end();
    if (FAILED(sink->Close())) return;
    ComPtr<ID2D1StrokeStyle> stroke;
    auto props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory->CreateStrokeStyle(props, nullptr, 0, &stroke);
    canvas.target()->DrawGeometry(path.Get(), canvas.brush(color), 2.5f * k, stroke.Get());
}
static void paintUsageRows(Canvas& dc, int width, int height, const UsageTableView& view) {
    if (view.empty) return;
    auto drawTokText = [&](const std::wstring& t, int x, int y, COLORREF color) {
        dc.text(t, {x, y, int(std::ceil(dc.textWidth(t, FUsage))) + 1, 20}, FUsage, color);
    };
    UsageCols cols = usageColumns(width);
    int colX = cols.x[3], colW = cols.w[3];
    int lineH = 20, gapV = 4;
    auto drawCell = [&](const wchar_t* t, int ci, int cy, COLORREF color) {
        dc.text(t, {cols.x[ci], cy, cols.w[ci], kUsageRowH}, FUsage, color, {TextAlign::Center});
    };
    auto measure = [&](const std::wstring& t) { return int(std::ceil(dc.textWidth(t, FUsage))); };
    int visible = usageVisibleRows(height);
    struct RowToks {
        std::wstring in, out, cache;
        int wIn = 0, wOut = 0, wCache = 0;
        bool hasCache = false;
    };
    std::vector<RowToks> toks;
    int maxStack = 0, iconD = 16, boxD = 14;
    int gap1 = 4, gap2 = 8;
    for (int i = 0; i < visible; ++i) {
        int idx = view.scroll + i;
        if (idx >= (int)view.rows.size()) break;
        const UsageRecord& r = view.rows[idx];
        RowToks t;
        t.in = usageComma(std::max<long long>(0, r.in - r.cache));
        t.out = usageComma(r.out);
        t.wIn = measure(t.in);
        t.wOut = measure(t.out);
        t.hasCache = r.cache > 0;
        if (t.hasCache) {
            t.cache = usageCacheTokens(r.cache);
            t.wCache = measure(t.cache);
        }
        maxStack = std::max(maxStack, std::max(iconD + gap1 + t.wIn + gap2 + iconD + gap1 + t.wOut,
                                               t.hasCache ? boxD + gap1 + t.wCache : 0));
        toks.push_back(t);
    }
    int x0 = colX + (colW - maxStack) / 2; // 统一左缘：跨行对齐（同 sub2api），整块列内居中
    for (int i = 0; i < (int)toks.size(); ++i) {
        const UsageRecord& r = view.rows[view.scroll + i];
        const RowToks& t = toks[i];
        int y = kUsageTop + i * kUsageRowH;
        int stackTop = y + (kUsageRowH - (t.hasCache ? lineH * 2 + gapV : lineH)) / 2;
        wchar_t ts[32]{};
        struct tm local {};
        time_t tsVal = r.ts;
        localtime_s(&local, &tsVal);
        wcsftime(ts, 32, L"%Y/%m/%d %H:%M:%S", &local);
        wchar_t credits[48]{};
        if (r.creditsKnown) swprintf(credits, 48, L"%.2f", r.creditsDelta);
        else swprintf(credits, 48, L"--");
        wchar_t joined[96]{};
        swprintf(joined, 96, L"%s%s", credits, r.merged ? L" (并发合计)" : L"");
        drawCell(ts, 0, y, C_TEXT);
        drawCell(toWide(r.model).c_str(), 1, y, C_TEXT);
        drawCell(toWide(r.endpoint).c_str(), 2, y, C_MUTED);
        // 行1：未缓存输入 = prompt_tokens − cached_tokens（对齐 sub2api input_tokens 口径）
        drawTokArrow(dc, x0, stackTop + (lineH - iconD) / 2, iconD, true, C_TOK_IN);
        drawTokText(t.in, x0 + iconD + gap1, stackTop, C_TOK_NUM);
        int upX = x0 + iconD + gap1 + t.wIn + gap2;
        drawTokArrow(dc, upX, stackTop + (lineH - iconD) / 2, iconD, false, C_TOK_OUT);
        drawTokText(t.out, upX + iconD + gap1, stackTop, C_TOK_NUM);
        if (t.hasCache) {
            int line2 = stackTop + lineH + gapV;
            drawTokBox(dc, x0, line2 + (lineH - boxD) / 2, boxD, C_TOK_BOX);
            drawTokText(t.cache, x0 + boxD + gap1, line2, C_TOK_CACHE);
        }
        drawCell(joined, 4, y, r.creditsKnown ? C_ACCENT : C_MUTED);
    }
}

void paintUsageTable(Canvas& dc, int width, int height, const UsageTableView& view) {
    paintPageHeader(dc, width, L"使用记录");
    paintPanel(dc, 40, 76, width - 80, height - 176);
    if (view.empty) {
        drawText(dc, L"暂无使用记录", { 40, 76, width - 80, height - 176 }, FBody,
                 C_MUTED, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }
    // 列带：按固定比例分配（几何源 usageColumns），避免端点与 TOKEN 之间出现大段空白
    UsageCols cols = usageColumns(width);
    const wchar_t* titles[5] = { L"时间", L"模型", L"端点", L"TOKEN", L"积分" };
    for (int i = 0; i < 5; ++i) {
        R r{ cols.x[i], 94, cols.w[i], 24 };
        drawText(dc, titles[i], r, FLabel, C_MUTED,
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    // 数据行（页内滚轮滚动）；sub2api 组合格两行 → 行高 kUsageRowH
    int visible = usageVisibleRows(height);
    for (int i = 0; i < visible; ++i) {
        int idx = view.scroll + i;
        if (idx >= (int)view.rows.size()) break;
        int y = kUsageTop + i * kUsageRowH;
        if (idx % 2 == 1) {
            dc.fill({41, y - 2, width - 82, kUsageRowH}, C_ROW_STRIPE);
        }
    }
    wchar_t info[64]{};
    swprintf(info, 64, L"第 %d / %d 页 · 每页 50 条", view.pageIndex + 1, view.totalPages);
    drawText(dc, info, { view.pagerX - 16 - 360, pagerY(height), 360, 34 }, FBody, C_MUTED,
             DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    paintUsageRows(dc, width, height, view);
}


} // namespace ui::visual
