#include "ui/UiDrawing.h"
#include "ui/Dpi.h"
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace ui::visual {
namespace {
void require(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(operation);
}
D2D1_COLOR_F color(COLORREF c) {
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f);
}
D2D1_RECT_F rect(R r) { return D2D1::RectF(float(r.x), float(r.y), float(r.x + r.w), float(r.y + r.h)); }
float fontSize(FontId id) {
    const auto& spec = kFontSpecs[id];
    return spec.unit == FontUnit::Points ? spec.size * 96.f / 72.f : float(spec.size);
}
struct DrawingResources {
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> write;
    ComPtr<IDWriteRenderingParams> textParams;
    ComPtr<ID2D1StrokeStyle> roundStroke;
    ComPtr<ID2D1StrokeStyle> focusStroke;
    ComPtr<IWICImagingFactory> imaging;
    ComPtr<IDWriteTextFormat> formats[FCount];
};
struct DrawingApartment {
    bool ownsCom;
    DrawingApartment() {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE) require(result, "Cannot initialize COM");
        ownsCom = SUCCEEDED(result);
    }
    ~DrawingApartment() { if (ownsCom) CoUninitialize(); }
};
std::unique_ptr<DrawingApartment> apartment;
std::unique_ptr<DrawingResources> resources;
// 原生输入控件仍引用这些 HFONT；隐藏窗口只能回收可重建的绘图资源。
HFONT inputFonts[FCount]{};
unsigned activeCanvases = 0;
bool releasePending = false;

struct Surface {
    HWND owner = nullptr;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    void prepare(HWND hwnd, HDC destination, const RECT& bounds, UINT dpiValue) {
        if (!target) {
            auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            ComPtr<ID2D1DCRenderTarget> nextTarget;
            ComPtr<ID2D1SolidColorBrush> nextBrush;
            require(resources->factory->CreateDCRenderTarget(&props, &nextTarget), "Cannot initialize Direct2D surface");
            require(nextTarget->CreateSolidColorBrush(color(C_TEXT), &nextBrush), "Cannot initialize UI brush");
            target = std::move(nextTarget);
            brush = std::move(nextBrush);
        }
        // DCRenderTarget 自带离屏缓冲，在 EndDraw 时提交到目标 DC，
        // 无需为每个页面/控件再保留一份 DIB 和内存 DC。
        require(target->BindDC(destination, &bounds), "Cannot bind Direct2D surface");
        owner = hwnd;
        target->SetDpi(float(dpiValue), float(dpiValue));
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target->SetTextRenderingParams(resources->textParams.Get());
    }
};
// UI 绘制在同一个线程顺序执行，只缓存一个可重绑的渲染目标。
// 重入绘制单独借用临时目标，不能重绑仍处于 BeginDraw 内的目标。
std::shared_ptr<Surface> cachedSurface;

ComPtr<IDWriteTextLayout> textLayout(std::wstring_view value, R bounds, FontId font, TextOptions options) {
    ComPtr<IDWriteTextLayout> layout;
    require(resources->write->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()),
        resources->formats[font].Get(), float(std::max(1, bounds.w)), float(std::max(1, bounds.h)), &layout),
        "Cannot create text layout");
    layout->SetTextAlignment(options.align == TextAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER :
        options.align == TextAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(options.vertical == TextVertical::Center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER :
        DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    layout->SetWordWrapping(options.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (options.ellipsis) {
        ComPtr<IDWriteInlineObject> ellipsis;
        require(resources->write->CreateEllipsisTrimmingSign(resources->formats[font].Get(), &ellipsis), "Cannot create text trimming");
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis.Get());
    }
    return layout;
}
} // namespace

void initializeDrawing() {
    if (resources) return;
    if (!apartment) apartment = std::make_unique<DrawingApartment>();
    auto next = std::make_unique<DrawingResources>();
    require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, next->factory.GetAddressOf()), "Direct2D initialization failed");
    // 独立 factory 的字体缓存随 UI 休眠释放；共享 factory 会继续保留进程级缓存。
    require(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(next->write.GetAddressOf())), "DirectWrite initialization failed");
    // 灰阶文字关闭额外对比度增强，保留小字号笔画边缘的平滑过渡。
    require(next->write->CreateCustomRenderingParams(2.2f, 0.f, 0.f, DWRITE_PIXEL_GEOMETRY_FLAT,
        DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &next->textParams), "Text antialiasing initialization failed");
    auto stroke = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    require(next->factory->CreateStrokeStyle(stroke, nullptr, 0, &next->roundStroke), "Stroke initialization failed");
    stroke.dashStyle = D2D1_DASH_STYLE_DOT;
    require(next->factory->CreateStrokeStyle(stroke, nullptr, 0, &next->focusStroke), "Focus stroke initialization failed");
    require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&next->imaging)), "Windows image encoder initialization failed");
    for (int i = 0; i < FCount; ++i) {
        const auto& spec = kFontSpecs[i];
        require(next->write->CreateTextFormat(spec.face, nullptr, static_cast<DWRITE_FONT_WEIGHT>(spec.weight),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize(static_cast<FontId>(i)), L"zh-CN",
            &next->formats[i]), "Font initialization failed");
    }
    resources = std::move(next);
}

void releaseSurface(HWND hwnd) {
    if (cachedSurface && cachedSurface->owner == hwnd) cachedSurface.reset();
}
void releaseDrawingCache() {
    cachedSurface.reset();
    // 隐藏消息可能重入绘制；最后一个 Canvas 结束后再释放它依赖的 factory。
    releasePending = activeCanvases != 0;
    if (!releasePending) resources.reset();
}
void releaseDrawing() {
    releaseDrawingCache();
    deleteFonts();
    apartment.reset();
}
void createFonts(UINT dpiValue) {
    deleteFonts();
    for (int i = 0; i < FCount; ++i) {
        const auto& spec = kFontSpecs[i];
        inputFonts[i] = CreateFontW(-MulDiv(spec.size, dpiValue, spec.unit == FontUnit::Points ? 72 : 96),
            0, 0, 0, spec.weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, spec.face);
    }
}
void deleteFonts() {
    for (auto& font : inputFonts) { if (font) DeleteObject(font); font = nullptr; }
}
HFONT ctrlFont(FontId id) { return inputFonts[id]; }

struct Canvas::Impl {
    std::shared_ptr<Surface> surface;
    HWND owner;
    float scale;
};
Canvas::Canvas(HWND owner, HDC destination, RECT pixels, COLORREF background) {
    if (pixels.right <= pixels.left || pixels.bottom <= pixels.top) return;
    initializeDrawing();
    auto surface = cachedSurface && cachedSurface.use_count() == 1
        ? cachedSurface : std::make_shared<Surface>();
    if (!cachedSurface) cachedSurface = surface;
    UINT dpiValue = dpi::forWindow(owner);
    surface->prepare(owner, destination, pixels, dpiValue);
    impl_ = std::make_unique<Impl>(Impl{surface, owner, dpiValue / 96.f});
    surface->target->BeginDraw();
    surface->target->Clear(color(background));
    ++activeCanvases;
}
Canvas::~Canvas() {
    if (!impl_) return;
    auto& surface = *impl_->surface;
    HRESULT result = surface.target->EndDraw();
    if (FAILED(result)) {
        surface.brush.Reset(); surface.target.Reset();
        InvalidateRect(impl_->owner, nullptr, FALSE);
    }
    impl_.reset();
    if (--activeCanvases == 0 && releasePending) releaseDrawingCache();
}
ID2D1DCRenderTarget* Canvas::target() const { return impl_ ? impl_->surface->target.Get() : nullptr; }
float Canvas::dpiScale() const { return impl_ ? impl_->scale : 1.f; }
ID2D1SolidColorBrush* Canvas::brush(COLORREF c) {
    impl_->surface->brush->SetColor(color(c));
    return impl_->surface->brush.Get();
}
void Canvas::fill(R r, COLORREF c) { if (target()) target()->FillRectangle(rect(r), brush(c)); }
void Canvas::roundRect(R r, float radius, COLORREF fillColor, COLORREF border, float stroke) {
    if (!target()) return;
    auto bounds = rect(r);
    float inset = stroke / 2;
    bounds.left += inset; bounds.top += inset; bounds.right -= inset; bounds.bottom -= inset;
    auto shape = D2D1::RoundedRect(bounds, radius, radius);
    target()->FillRoundedRectangle(shape, brush(fillColor));
    if (border != CLR_INVALID) target()->DrawRoundedRectangle(shape, brush(border), stroke);
}
void Canvas::ellipse(R r, COLORREF c) {
    if (target()) target()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.x + r.w / 2.f, r.y + r.h / 2.f),
        r.w / 2.f, r.h / 2.f), brush(c));
}
void Canvas::line(Point a, Point b, COLORREF c, float width) {
    if (target()) target()->DrawLine(D2D1::Point2F(a.x, a.y), D2D1::Point2F(b.x, b.y), brush(c), width, resources->roundStroke.Get());
}
void Canvas::polyline(const Point* points, size_t count, COLORREF c, float width, bool close, bool filled) {
    if (!target() || count < 2) return;
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    require(resources->factory->CreatePathGeometry(&path), "Cannot create vector path");
    require(path->Open(&sink), "Cannot open vector path");
    sink->BeginFigure(D2D1::Point2F(points[0].x, points[0].y), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < count; ++i) sink->AddLine(D2D1::Point2F(points[i].x, points[i].y));
    sink->EndFigure(close ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    require(sink->Close(), "Cannot close vector path");
    if (filled) target()->FillGeometry(path.Get(), brush(c));
    else target()->DrawGeometry(path.Get(), brush(c), width, resources->roundStroke.Get());
}
void Canvas::icon(ButtonIcon icon, R bounds, COLORREF c) {
    if (!target() || icon == ButtonIcon::None) return;
    // Lucide Icons, Copyright (c) 2026 Lucide Icons and Contributors (ISC).
    // Permission to use, copy, modify, and/or distribute this software for any purpose
    // with or without fee is granted when this notice is retained. The software is
    // provided "as is" without warranty; the authors are not liable for damages.
    // 与 1owtool 一致的 copy / refresh-cw 24x24 路径，使用现有 Direct2D 渲染。
    const float side = float(std::max(12, std::min(bounds.w, bounds.h) - 10));
    const float unit = side / 24.f;
    const float x = bounds.x + (bounds.w - side) * 0.5f;
    const float y = bounds.y + (bounds.h - side) * 0.5f;
    const auto point = [=](float px, float py) { return D2D1::Point2F(x + px * unit, y + py * unit); };
    const float stroke = std::max(1.4f, 2.f * unit);
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    require(resources->factory->CreatePathGeometry(&path), "Cannot create button icon");
    require(path->Open(&sink), "Cannot open button icon");
    if (icon == ButtonIcon::Copy) {
        auto front = D2D1::RoundedRect(D2D1::RectF(x + 8 * unit, y + 2 * unit, x + 22 * unit, y + 16 * unit), 2 * unit, 2 * unit);
        target()->DrawRoundedRectangle(front, brush(c), stroke, resources->roundStroke.Get());
        sink->BeginFigure(point(8, 8), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(point(4, 8));
        sink->AddBezier(D2D1::BezierSegment(point(2.9f, 8), point(2, 8.9f), point(2, 10)));
        sink->AddLine(point(2, 20));
        sink->AddBezier(D2D1::BezierSegment(point(2, 21.1f), point(2.9f, 22), point(4, 22)));
        sink->AddLine(point(14, 22));
        sink->AddBezier(D2D1::BezierSegment(point(15.1f, 22), point(16, 21.1f), point(16, 20)));
        sink->AddLine(point(16, 16));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
    } else {
        constexpr float diagonal = 9.f * 0.70710678f;
        sink->BeginFigure(point(3, 12), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(point(12 + diagonal, 12 - diagonal), D2D1::SizeF(9 * unit, 9 * unit), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->BeginFigure(point(16, 8), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(point(21, 8)); sink->AddLine(point(21, 3));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->BeginFigure(point(21, 12), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(point(12 - diagonal, 12 + diagonal), D2D1::SizeF(9 * unit, 9 * unit), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->BeginFigure(point(8, 16), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(point(3, 16)); sink->AddLine(point(3, 21));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
    }
    require(sink->Close(), "Cannot close button icon");
    target()->DrawGeometry(path.Get(), brush(c), stroke, resources->roundStroke.Get());
}

void Canvas::nativeIcon(HICON icon, R bounds) {
    if (!target() || !icon) return;
    ComPtr<IWICBitmap> source;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<ID2D1Bitmap> bitmap;
    require(resources->imaging->CreateBitmapFromHICON(icon, &source), "Cannot decode application icon");
    require(resources->imaging->CreateFormatConverter(&converter), "Cannot create icon converter");
    require(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Cannot convert application icon");
    require(target()->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap), "Cannot create application bitmap");
    target()->DrawBitmap(bitmap.Get(), rect(bounds), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Canvas::text(std::wstring_view value, R r, FontId font, COLORREF c, TextOptions options) {
    if (!target() || value.empty() || r.w <= 0 || r.h <= 0) return;
    auto layout = textLayout(value, r, font, options);
    target()->DrawTextLayout(D2D1::Point2F(float(r.x), float(r.y)), layout.Get(), brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
float Canvas::textWidth(std::wstring_view value, FontId font) {
    auto layout = textLayout(value, {0, 0, 100000, 1000}, font, {TextAlign::Left, TextVertical::Top, false, false});
    DWRITE_TEXT_METRICS metrics{};
    require(layout->GetMetrics(&metrics), "Cannot measure text");
    return metrics.widthIncludingTrailingWhitespace;
}
void Canvas::focus(R r, COLORREF c) {
    if (!target()) return;
    auto bounds = rect(r);
    bounds.left += 2; bounds.top += 2; bounds.right -= 2; bounds.bottom -= 2;
    target()->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 3, 3), brush(c), 1, resources->focusStroke.Get());
}
void Canvas::pushClip(R r) { if (target()) target()->PushAxisAlignedClip(rect(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE); }
void Canvas::popClip() { if (target()) target()->PopAxisAlignedClip(); }

void Canvas::editText(HWND edit) {
    if (!target()) return;
    int length = GetWindowTextLengthW(edit);
    std::wstring value(size_t(length) + 1, L'\0');
    GetWindowTextW(edit, value.data(), int(value.size())); value.resize(size_t(length));
    if (value.empty()) return;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    LOGFONTW native{}; GetObjectW(font, sizeof(native), &native);
    ComPtr<IDWriteTextFormat> format;
    float pxPerDip = dpiScale();
    require(resources->write->CreateTextFormat(native.lfFaceName, nullptr,
        static_cast<DWRITE_FONT_WEIGHT>(native.lfWeight), DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        float(std::abs(native.lfHeight)) / pxPerDip, L"zh-CN", &format), "Cannot create input typography");
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    // The native edit engine owns selection, undo, IME and caret positions. Match
    // its advance widths while DirectWrite remains the only glyph rasterizer.
    require(resources->write->CreateGdiCompatibleTextLayout(value.data(), UINT32(value.size()), format.Get(),
        100000, 1000, pxPerDip, nullptr, FALSE, &layout), "Cannot create input text layout");
    int first = int(SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0));
    first = std::clamp(first, 0, length);
    float firstX = 0, firstY = 0; DWRITE_HIT_TEST_METRICS hit{};
    layout->HitTestTextPosition(first, FALSE, &firstX, &firstY, &hit);
    LRESULT nativePosition = SendMessageW(edit, EM_POSFROMCHAR, first, 0);
    float originX = float(static_cast<short>(LOWORD(nativePosition))) / pxPerDip - firstX;
    float originY = float(static_cast<short>(HIWORD(nativePosition))) / pxPerDip;
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    bool selection = GetFocus() == edit && start != end;
    std::vector<DWRITE_HIT_TEST_METRICS> ranges;
    if (selection) {
        UINT32 count = 0;
        layout->HitTestTextRange(start, end - start, originX, originY, nullptr, 0, &count);
        ranges.resize(count);
        layout->HitTestTextRange(start, end - start, originX, originY, ranges.data(), count, &count);
        for (const auto& range : ranges) target()->FillRectangle(
            D2D1::RectF(range.left, range.top, range.left + range.width, range.top + range.height), brush(GetSysColor(COLOR_HIGHLIGHT)));
    }
    auto origin = D2D1::Point2F(originX, originY);
    target()->DrawTextLayout(origin, layout.Get(), brush(IsWindowEnabled(edit) ? C_TEXT : C_DISABLED_TEXT));
    for (const auto& range : ranges) {
        target()->PushAxisAlignedClip(D2D1::RectF(range.left, range.top, range.left + range.width, range.top + range.height), D2D1_ANTIALIAS_MODE_ALIASED);
        target()->DrawTextLayout(origin, layout.Get(), brush(GetSysColor(COLOR_HIGHLIGHTTEXT)));
        target()->PopAxisAlignedClip();
    }
}

int s(HWND hwnd, int value) { return dpi::scale(hwnd, value); }
int logicalWidth(HWND hwnd) { RECT r{}; GetClientRect(hwnd, &r); return MulDiv(r.right, 96, dpi::forWindow(hwnd)); }
int logicalHeight(HWND hwnd) { RECT r{}; GetClientRect(hwnd, &r); return MulDiv(r.bottom, 96, dpi::forWindow(hwnd)); }
void place(HWND parent, HWND control, R r, UINT flags) {
    if (!control) return;
    RECT current{}; GetWindowRect(control, &current);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&current), 2);
    RECT next{ s(parent, r.x), s(parent, r.y), s(parent, r.w), s(parent, r.h) };
    if (current.left != next.left || current.top != next.top || current.right - current.left != next.right ||
        current.bottom - current.top != next.bottom) SetWindowPos(control, nullptr, next.left, next.top, next.right, next.bottom, flags);
}
RECT physRect(HWND page, int x, int y, int w, int h) { return {s(page, x), s(page, y), s(page, x + w), s(page, y + h)}; }
std::wstring toWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), out.data(), size); return out;
}
std::string toUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), size, nullptr, nullptr); return out;
}
void drawText(Canvas& canvas, const wchar_t* value, R r, FontId font, COLORREF c, UINT flags) {
    TextOptions options;
    options.align = flags & DT_CENTER ? TextAlign::Center : flags & DT_RIGHT ? TextAlign::Right : TextAlign::Left;
    options.vertical = flags & DT_VCENTER ? TextVertical::Center : TextVertical::Top;
    options.wrap = !(flags & DT_SINGLELINE);
    canvas.text(value, r, font, c, options);
}
void drawText(Canvas& canvas, const std::wstring& value, R r, FontId font, COLORREF c, UINT flags) {
    drawText(canvas, value.c_str(), r, font, c, flags);
}
void drawDivider(Canvas& canvas, int x1, int y, int x2) { canvas.line({float(x1), float(y)}, {float(x2), float(y)}, C_BORDER); }
void paintPanel(Canvas& canvas, int x, int y, int w, int h) { canvas.roundRect({x, y, w, h}, 5, C_SURFACE, C_BORDER); }
void paintGroupLabel(Canvas& canvas, int x, int y, const wchar_t* value) { canvas.text(value, {x, y, 300, 18}, FLabel, C_MUTED, {TextAlign::Left, TextVertical::Top}); }
void paintPageHeader(Canvas& canvas, int, const wchar_t* title) { canvas.text(title, {40, 26, 400, 30}, FTitle, C_TEXT); }

bool savePng(HBITMAP bitmap, const std::wstring& path) {
    initializeDrawing();
    ComPtr<IWICBitmap> source; ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> properties;
    auto* imaging = resources->imaging.Get();
    if (FAILED(imaging->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source)) ||
        FAILED(imaging->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &properties)) || FAILED(frame->Initialize(properties.Get()))) return false;
    UINT w = 0, h = 0; source->GetSize(&w, &h);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    return SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
        SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}
} // namespace ui::visual
