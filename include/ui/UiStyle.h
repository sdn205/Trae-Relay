#pragma once
#include <windows.h>

namespace ui::visual {

// ===== 设计 token（信号台 / Signal Console）=====
// Paper  冷浅灰绿纸面   Ink    墨绿黑（仪器带/标题）
// Panel  面板白         Line   发丝边
// Signal 中继信号青绿（在线/选中/主操作），请求中使用蓝色。
inline constexpr COLORREF C_INK = RGB(21, 32, 30);
inline constexpr COLORREF C_INK_PRESS = RGB(44, 60, 55);
inline constexpr COLORREF C_CANVAS = RGB(237, 241, 240);
inline constexpr COLORREF C_SURFACE = RGB(252, 253, 252);
inline constexpr COLORREF C_BORDER = RGB(217, 224, 221);
inline constexpr COLORREF C_TEXT = RGB(24, 34, 32);
inline constexpr COLORREF C_MUTED = RGB(98, 112, 107);
inline constexpr COLORREF C_BAR_TEXT = RGB(240, 244, 242);
inline constexpr COLORREF C_BAR_MUTED = RGB(143, 160, 154);
inline constexpr COLORREF C_BUS_TRACK = RGB(47, 63, 59);
inline constexpr COLORREF C_ACCENT = RGB(16, 158, 120);
inline constexpr COLORREF C_ACCENT_SOFT = RGB(221, 242, 235);
inline constexpr COLORREF C_ACCENT_INK = RGB(11, 127, 96);
inline constexpr COLORREF C_CORAL = RGB(206, 74, 58);
inline constexpr COLORREF C_AMBER = RGB(169, 112, 26);
inline constexpr COLORREF C_REQUEST = RGB(37, 99, 235);
inline constexpr COLORREF C_QUEUED = RGB(234, 179, 8);
inline constexpr COLORREF C_DISABLED_TEXT = RGB(156, 168, 163);
inline constexpr COLORREF C_DISABLED_ARROW = RGB(180, 190, 186);
inline constexpr COLORREF C_DISABLED_ACCENT = RGB(150, 200, 185);
inline constexpr COLORREF C_SELECTED_PRESS = RGB(205, 234, 224);
inline constexpr COLORREF C_BUTTON_PRESS = RGB(231, 236, 234);
inline constexpr COLORREF C_CHECK_MARK = RGB(255, 255, 255);
inline constexpr COLORREF C_ROW_STRIPE = RGB(246, 249, 248);

// sub2api（Wei-Shaw/sub2api UsageTable.vue）token 单元格配色，浅色态取 Tailwind v3 默认调色板：
// 输入/输出数字 gray-900，↓ emerald-500，↑ violet-500，缓存图标 sky-500、缓存数字 sky-600
inline constexpr COLORREF C_TOK_NUM = RGB(0x11, 0x18, 0x27);
inline constexpr COLORREF C_TOK_IN = RGB(0x10, 0xb9, 0x81);
inline constexpr COLORREF C_TOK_OUT = RGB(0x8b, 0x5c, 0xf6);
inline constexpr COLORREF C_TOK_BOX = RGB(0x0e, 0xa5, 0xe9);
inline constexpr COLORREF C_TOK_CACHE = RGB(0x02, 0x84, 0xc7);

struct R {
    int x;
    int y;
    int w;
    int h;
};

enum FontId { FBody, FSmall, FLabel, FTitle, FMetric, FBrand, FTab, FValue, FUsage, FMetricSemibold, FCount };
constexpr wchar_t kUiFontFace[] = L"Segoe UI";
enum class FontUnit { Points, Pixels };
struct FontSpec {
    int size;
    int weight;
    const wchar_t* face;
    FontUnit unit = FontUnit::Points;
};
constexpr FontSpec kFontSpecs[FCount] = {
    { 10, FW_NORMAL, kUiFontFace },                    // FBody
    { 9, FW_NORMAL, kUiFontFace },                     // FSmall
    { 10, FW_SEMIBOLD, L"Microsoft YaHei UI" },        // FLabel
    { 17, FW_SEMIBOLD, L"Segoe UI Variable Display" }, // FTitle
    { 20, FW_NORMAL, kUiFontFace },                    // FMetric
    { 11, FW_NORMAL, kUiFontFace },                    // FBrand
    { 10, FW_SEMIBOLD, L"Microsoft YaHei UI" },        // FTab
    { 10, FW_NORMAL, kUiFontFace },                    // FValue：端点、密钥、端口等
    { 14, FW_NORMAL, kUiFontFace, FontUnit::Pixels },    // FUsage：表格正文
    { 20, FW_SEMIBOLD, kUiFontFace },                  // FMetricSemibold
};

// 所有尺寸均为 96 DPI 逻辑像素，由 Canvas 或窗口布局在边界统一换算。
struct FieldMetrics {
    static constexpr int height = 30;
    static constexpr int editInsetX = 10, editInsetY = 4, editHeight = 22;
    static constexpr int comboItemHeight = 26;
    static constexpr int comboItemInsetLeft = 12, comboItemInsetRight = 8;
    static constexpr int comboArrowRight = 15;
    static constexpr int comboTextLeft = 10, comboTextRight = 28;
    static constexpr int comboArrowHalfWidth = 4;
    static constexpr int comboDropHeight = 200;
};
struct ButtonMetrics {
    static constexpr int corner = 3;
    static constexpr int selectedPageCorner = 5;
    static constexpr int checkCorner = 2;
};
} // namespace ui::visual
