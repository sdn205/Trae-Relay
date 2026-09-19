#pragma once
#include "ui/UiStyle.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <string_view>

namespace ui::visual {
using Microsoft::WRL::ComPtr;
struct Point { float x, y; };
enum class ButtonIcon { None, Copy, Regenerate };
enum class TextAlign { Left, Center, Right };
enum class TextVertical { Top, Center };
struct TextOptions {
    TextAlign align = TextAlign::Left;
    TextVertical vertical = TextVertical::Center;
    bool wrap = false;
    bool ellipsis = true;
};
void initializeDrawing();
void releaseDrawing();
void releaseSurface(HWND hwnd);
// 最小化/隐藏时回收画布、排版和字体缓存；保留原生输入控件引用的 HFONT。
// 下次绘制按需重建，重入调用会延迟到所有 Canvas 结束后释放。
void releaseDrawingCache();
int s(HWND hwnd, int value);
int logicalWidth(HWND hwnd);
int logicalHeight(HWND hwnd);
void place(HWND parent, HWND control, R rect, UINT flags = SWP_NOZORDER | SWP_NOACTIVATE);
RECT physRect(HWND page, int x, int y, int w, int h);
std::wstring toWide(const std::string& value);
std::string toUtf8(const std::wstring& value);
// HFONT is used only by native input for caret and selection metrics.
void createFonts(UINT dpiValue);
void deleteFonts();
HFONT ctrlFont(FontId id);

// One Direct2D/DirectWrite renderer; all public geometry uses 96-DPI DIPs.
class Canvas {
public:
    Canvas(HWND owner, HDC destination, RECT pixels, COLORREF background);
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    void fill(R rect, COLORREF color);
    void roundRect(R rect, float radius, COLORREF fill, COLORREF border = CLR_INVALID, float stroke = 1);
    void ellipse(R rect, COLORREF color);
    void line(Point from, Point to, COLORREF color, float width = 1);
    void polyline(const Point* points, size_t count, COLORREF color, float width = 1, bool close = false, bool fill = false);
    void icon(ButtonIcon icon, R bounds, COLORREF color);
    void nativeIcon(HICON icon, R bounds);
    void text(std::wstring_view value, R rect, FontId font, COLORREF color, TextOptions options = {});
    float textWidth(std::wstring_view value, FontId font);
    void focus(R rect, COLORREF color);
    void pushClip(R rect);
    void popClip();
    void editText(HWND edit);
    ID2D1DCRenderTarget* target() const;
    ID2D1SolidColorBrush* brush(COLORREF color);
    float dpiScale() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void drawText(Canvas& canvas, const wchar_t* text, R rect, FontId font, COLORREF color, UINT format);
void drawText(Canvas& canvas, const std::wstring& text, R rect, FontId font, COLORREF color, UINT format);
void drawDivider(Canvas& canvas, int x1, int y, int x2);
void paintPanel(Canvas& canvas, int x, int y, int w, int h);
void paintGroupLabel(Canvas& canvas, int x, int y, const wchar_t* text);
void paintPageHeader(Canvas& canvas, int width, const wchar_t* title);
bool savePng(HBITMAP bitmap, const std::wstring& path);
} // namespace ui::visual
