#include "ui/UiControls.h"
#include "ui/Dpi.h"
#include <algorithm>
#include <uxtheme.h>

namespace ui::visual {
namespace {
std::wstring windowText(HWND hwnd);
void sizeCombo(HWND hwnd) {
    const int height = s(hwnd, FieldMetrics::height);
    SendMessageW(hwnd, CB_SETITEMHEIGHT, 0, s(hwnd, FieldMetrics::comboItemHeight));
    RECT client{};
    GetClientRect(hwnd, &client);
    int itemHeight = static_cast<int>(SendMessageW(hwnd, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
    SendMessageW(hwnd, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
        std::max(1, itemHeight + height - static_cast<int>(client.bottom)));
}
constexpr CtrlDef kCtrlDefs[] = {
    { IDC_NAV_STATUS, Ck::NavTab, FTab, PAGE_STATUS },
    { IDC_NAV_USAGE, Ck::NavTab, FTab, PAGE_USAGE },
    { IDC_NAV_SETTINGS, Ck::NavTab, FTab, PAGE_SETTINGS },
    { IDC_ED_BASEURL, Ck::Edit, FValue, PAGE_STATUS, baseUrlBox },
    { IDC_ED_APIKEY, Ck::Edit, FValue, PAGE_STATUS, apiKeyBox },
    { IDC_BTN_COPYURL, Ck::Btn, FBody, PAGE_STATUS, nullptr, ButtonIcon::Copy },
    { IDC_BTN_COPY, Ck::Btn, FBody, PAGE_STATUS, nullptr, ButtonIcon::Copy },
    { IDC_BTN_RESETKEY, Ck::Btn, FBody, PAGE_STATUS, nullptr, ButtonIcon::Regenerate },
    { IDC_CHK_ANYKEY, Ck::Check, FBody, PAGE_STATUS },
    { IDC_ST_KEYHINT, Ck::Static, FSmall, PAGE_STATUS },
    { IDC_BTN_CREDITS, Ck::Btn, FBody, PAGE_STATUS },
    { IDC_BTN_CHECKIN, Ck::PrimaryBtn, FBody, PAGE_STATUS },
    { IDC_CB_MODEL, Ck::Combo, FValue, PAGE_STATUS, modelComboBox },
    { IDC_BTN_EFF0, Ck::EffortBtn, FBody, PAGE_STATUS },
    { IDC_BTN_EFF1, Ck::EffortBtn, FBody, PAGE_STATUS },
    { IDC_BTN_EFF2, Ck::EffortBtn, FBody, PAGE_STATUS },
    { IDC_CHK_MAX, Ck::Check, FBody, PAGE_STATUS },
    { IDC_ST_MODELHINT, Ck::Static, FSmall, PAGE_STATUS },
    { IDC_ED_PORT, Ck::Edit, FValue, PAGE_SETTINGS, portBox },
    { IDC_CHK_LAN, Ck::Check, FBody, PAGE_SETTINGS },
    { IDC_CHK_AUTOSTART, Ck::Check, FBody, PAGE_SETTINGS },
    { IDC_CHK_STARTMIN, Ck::Check, FBody, PAGE_SETTINGS },
    { IDC_CHK_MINCLOSE, Ck::Check, FBody, PAGE_SETTINGS },
    { IDC_CHK_CHECKIN, Ck::Check, FBody, PAGE_SETTINGS },
    { IDC_ED_HOUR, Ck::Edit, FValue, PAGE_SETTINGS, hourBox },
    { IDC_ED_MINUTE, Ck::Edit, FValue, PAGE_SETTINGS, minuteBox },
    { IDC_CHK_LOGGING, Ck::Check, FBody, PAGE_SETTINGS },
    { IDC_BTN_PAGE_PREV, Ck::PagerBtn, FBody, PAGE_USAGE },
    { IDC_BTN_PAGE_NEXT, Ck::PagerBtn, FBody, PAGE_USAGE },
    { IDC_PAGER_SLOT0, Ck::PagerSlot, FBody, PAGE_USAGE },
    { IDC_PAGER_SLOT0 + 1, Ck::PagerSlot, FBody, PAGE_USAGE },
    { IDC_PAGER_SLOT0 + 2, Ck::PagerSlot, FBody, PAGE_USAGE },
    { IDC_PAGER_SLOT0 + 3, Ck::PagerSlot, FBody, PAGE_USAGE },
    { IDC_PAGER_SLOT0 + 4, Ck::PagerSlot, FBody, PAGE_USAGE },
    { IDC_PAGER_SLOT0 + 5, Ck::PagerSlot, FBody, PAGE_USAGE },
    { IDC_PAGER_SLOT0 + 6, Ck::PagerSlot, FBody, PAGE_USAGE },
};
}

const CtrlDef* ctrlDef(int id) {
    for (const auto& def : kCtrlDefs) if (def.id == id) return &def;
    return nullptr;
}

FontId controlFontId(int id) { const auto* def = ctrlDef(id); return def ? def->font : FBody; }
void setControlText(HWND control, const wchar_t* text) {
    if (control && windowText(control) != text) SetWindowTextW(control, text);
}

void enableControl(HWND control, bool enabled) {
    if (control && (IsWindowEnabled(control) != FALSE) != enabled) EnableWindow(control, enabled);
}

void applyControlFonts(HWND hwnd) {
    EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
        const auto* def = ctrlDef(GetDlgCtrlID(child));
        if (def) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(ctrlFont(def->font)), TRUE);
            if (def->kind == Ck::Combo) sizeCombo(child);
        }
        return TRUE;
    }, 0);
}

void placeField(HWND page, int id) {
    const auto* def = ctrlDef(id);
    if (!def || !def->fieldBox) return;
    R box = def->fieldBox(logicalWidth(page));
    place(page, GetDlgItem(page, id), def->kind == Ck::Combo ? comboCtrl(box, FieldMetrics::comboDropHeight) : editCtrl(box));
}

void invalidateField(HWND page, HWND control) {
    const auto* def = ctrlDef(GetDlgCtrlID(control));
    if (!def || !def->fieldBox) return;
    R box = def->fieldBox(logicalWidth(page));
    RECT frame = physRect(page, box.x, box.y, box.w, box.h);
    InvalidateRect(page, &frame, FALSE);
    InvalidateRect(control, nullptr, FALSE);
}

void measureControl(HWND parent, MEASUREITEMSTRUCT* measure) {
    if (measure->CtlType == ODT_COMBOBOX) measure->itemHeight = s(parent, FieldMetrics::comboItemHeight);
}

LRESULT colorControl(UINT, HDC dc, HWND, HBRUSH, HBRUSH surface) {
    SetBkColor(dc, C_SURFACE);
    SetTextColor(dc, C_TEXT);
    return reinterpret_cast<LRESULT>(surface);
}

namespace {
R boundsFor(HWND hwnd, RECT pixels) {
    UINT dpiValue = dpi::forWindow(hwnd);
    return {0, 0, MulDiv(pixels.right - pixels.left, 96, dpiValue),
                  MulDiv(pixels.bottom - pixels.top, 96, dpiValue)};
}
std::wstring windowText(HWND hwnd) {
    std::wstring text(GetWindowTextLengthW(hwnd) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
    text.resize(wcslen(text.c_str()));
    return text;
}
void comboLabel(HWND hwnd, Canvas& canvas, int index, R bounds, COLORREF color) {
    std::wstring text;
    LRESULT length = SendMessageW(hwnd, CB_GETLBTEXTLEN, index, 0);
    if (index >= 0 && length != CB_ERR) {
        text.resize(static_cast<size_t>(length) + 1);
        SendMessageW(hwnd, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<size_t>(length));
    }
    canvas.text(text, bounds, controlFontId(GetDlgCtrlID(hwnd)), color);
}
void paintCombo(HWND hwnd, HDC destination) {
    RECT pixels{};
    GetClientRect(hwnd, &pixels);
    Canvas canvas(hwnd, destination, pixels, C_SURFACE);
    R box = boundsFor(hwnd, pixels);
    canvas.roundRect(box, 3, C_SURFACE, GetFocus() == hwnd ? C_ACCENT : C_BORDER);
    bool disabled = !IsWindowEnabled(hwnd);
    int selected = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
    comboLabel(hwnd, canvas, selected, {FieldMetrics::comboTextLeft, 0,
        box.w - FieldMetrics::comboTextLeft - FieldMetrics::comboTextRight, box.h},
        disabled ? C_DISABLED_TEXT : C_TEXT);
    float x = float(box.w - FieldMetrics::comboArrowRight), y = box.h / 2.f;
    float a = float(FieldMetrics::comboArrowHalfWidth);
    Point arrow[] = {{x - a, y - a / 2}, {x + a, y - a / 2}, {x, y + a / 2 + 1}};
    canvas.polyline(arrow, 3, disabled ? C_DISABLED_ARROW : C_MUTED, 1, true, true);
}
void paintStatic(HWND hwnd, HDC destination) {
    RECT pixels{};
    GetClientRect(hwnd, &pixels);
    const CtrlDef* def = ctrlDef(GetDlgCtrlID(hwnd));
    bool accent = def && def->kind == Ck::StaticAccent;
    Canvas canvas(hwnd, destination, pixels, accent ? C_CANVAS : C_SURFACE);
    bool right = (GetWindowLongPtrW(hwnd, GWL_STYLE) & SS_TYPEMASK) == SS_RIGHT;
    canvas.text(windowText(hwnd), boundsFor(hwnd, pixels), controlFontId(GetDlgCtrlID(hwnd)),
        accent ? C_ACCENT_INK : C_MUTED, {right ? TextAlign::Right : TextAlign::Left, TextVertical::Top, true});
}
void paintEdit(HWND hwnd, HDC destination) {
    RECT pixels{};
    GetClientRect(hwnd, &pixels);
    Canvas canvas(hwnd, destination, pixels, C_SURFACE);
    canvas.editText(hwnd);
}

LRESULT CALLBACK inputSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto kind = static_cast<Ck>(data);
    if (msg == WM_NCDESTROY) {
        releaseSurface(hwnd);
        RemoveWindowSubclass(hwnd, inputSubclass, id);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT || msg == WM_PRINT) {
        PAINTSTRUCT ps{};
        bool painting = msg == WM_PAINT;
        if (painting && kind == Ck::Edit) HideCaret(hwnd);
        HDC dc = painting ? BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wp);
        if (kind == Ck::Combo) paintCombo(hwnd, dc);
        else if (kind == Ck::Edit) paintEdit(hwnd, dc);
        else paintStatic(hwnd, dc);
        if (painting) EndPaint(hwnd, &ps);
        if (painting && kind == Ck::Edit) ShowCaret(hwnd);
        return 0;
    }
    LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
    if (kind == Ck::Edit) {
        switch (msg) {
        case WM_CHAR: case WM_KEYDOWN: case WM_KEYUP: case WM_SETTEXT:
        case WM_SETFOCUS: case WM_KILLFOCUS: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_MOUSEMOVE: case EM_SETSEL: case EM_REPLACESEL: case EM_SCROLLCARET:
        case WM_CUT: case WM_PASTE: case WM_CLEAR: case WM_UNDO: case EM_UNDO:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
    }
    return result;
}
LRESULT CALLBACK buttonSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_NCDESTROY) {
        releaseSurface(hwnd);
        RemoveWindowSubclass(hwnd, buttonSubclass, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
}

HWND makeControl(HWND parent, const wchar_t* text, DWORD style, int id) {
    const CtrlDef* def = ctrlDef(id);
    if (!def) return nullptr;
    bool label = def->kind == Ck::Static || def->kind == Ck::StaticAccent;
    const wchar_t* name = def->kind == Ck::Edit ? L"EDIT" : def->kind == Ck::Combo ? L"COMBOBOX" : label ? L"STATIC" : L"BUTTON";
    if (!label) style |= WS_TABSTOP;
    HWND control = CreateWindowExW(0, name, text, WS_CHILD | WS_CLIPSIBLINGS | style, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ctrlFont(def->font)), TRUE);
    SetWindowTheme(control, def->kind == Ck::Edit || def->kind == Ck::Combo ? L"" : L"Explorer", L"");
    if (def->kind == Ck::Combo) {
        SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) & ~WS_BORDER);
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(control, &info)) SetWindowTheme(info.hwndList, L"Explorer", nullptr);
        sizeCombo(control);
    }
    if (def->kind == Ck::Combo || def->kind == Ck::Edit || label)
        SetWindowSubclass(control, inputSubclass, 1, static_cast<DWORD_PTR>(def->kind));
    else SetWindowSubclass(control, buttonSubclass, 1, 0);
    return control;
}

void paintField(Canvas& canvas, HWND page, int id) {
    const CtrlDef* def = ctrlDef(id);
    if (!def || !def->fieldBox || def->kind == Ck::Combo) return;
    R box = def->fieldBox(logicalWidth(page));
    canvas.roundRect(box, 3, C_SURFACE, GetFocus() == GetDlgItem(page, id) ? C_ACCENT : C_BORDER);
}

void drawOwnerCombo(const DRAWITEMSTRUCT* item) {
    if (!item || item->itemID == static_cast<UINT>(-1)) return;
    bool selected = (item->itemState & ODS_SELECTED) != 0;
    Canvas canvas(item->hwndItem, item->hDC, item->rcItem, selected ? C_ACCENT_SOFT : C_SURFACE);
    R box = boundsFor(item->hwndItem, item->rcItem);
    comboLabel(item->hwndItem, canvas, static_cast<int>(item->itemID),
        {FieldMetrics::comboItemInsetLeft, 0, box.w - FieldMetrics::comboItemInsetLeft - FieldMetrics::comboItemInsetRight, box.h},
        selected ? C_ACCENT_INK : C_TEXT);
}

void drawOwnerButton(const DRAWITEMSTRUCT* item, ButtonState state) {
    if (!item || item->CtlType != ODT_BUTTON) return;
    const CtrlDef* def = ctrlDef(static_cast<int>(item->CtlID));
    if (!def) return;
    Ck kind = def->kind;
    COLORREF base = kind == Ck::NavTab ? C_INK : kind == Ck::PagerSlot || kind == Ck::PagerBtn ? C_CANVAS : C_SURFACE;
    Canvas canvas(item->hwndItem, item->hDC, item->rcItem, base);
    R box = boundsFor(item->hwndItem, item->rcItem);
    bool pressed = (item->itemState & ODS_SELECTED) != 0;
    bool disabled = (item->itemState & ODS_DISABLED) != 0;
    std::wstring label = windowText(item->hwndItem);
    if (kind == Ck::NavTab) {
        if (pressed && !state.selected) canvas.fill(box, C_INK_PRESS);
        canvas.text(label, box, def->font, state.selected ? C_BAR_TEXT : C_BAR_MUTED, {TextAlign::Center});
        if (state.selected) canvas.fill({20, box.h - 3, box.w - 40, 2}, C_ACCENT);
        return;
    }
    if (kind == Ck::Check) {
        int y = box.h / 2;
        R mark{1, y - 8, 16, 16};
        canvas.roundRect(mark, ButtonMetrics::checkCorner, disabled ? C_CANVAS : state.checked ? pressed ? C_ACCENT_INK : C_ACCENT : C_SURFACE,
            disabled || !state.checked ? pressed ? C_ACCENT_INK : C_BORDER : CLR_INVALID);
        if (state.checked) {
            Point tick[] = {{5.f, y - 1.f}, {8.f, y + 2.f}, {13.f, y - 4.f}};
            canvas.polyline(tick, 3, disabled ? C_DISABLED_TEXT : C_CHECK_MARK, 2);
        }
        canvas.text(label, {26, 0, box.w - 26, box.h}, def->font,
            disabled ? C_DISABLED_TEXT : C_TEXT,
            {def->id == IDC_CHK_ANYKEY ? TextAlign::Right : TextAlign::Left});
        return;
    }
    bool current = kind == Ck::PagerSlot && state.selected;
    bool primary = kind == Ck::PrimaryBtn;
    bool effort = kind == Ck::EffortBtn && state.selected;
    COLORREF background = C_SURFACE, border = C_BORDER, text = C_TEXT;
    if (current) { background = pressed ? C_INK_PRESS : C_ACCENT; border = C_ACCENT; text = C_BAR_TEXT; }
    else if (primary) { background = disabled ? C_DISABLED_ACCENT : pressed ? C_ACCENT_INK : C_ACCENT; border = background; text = C_CHECK_MARK; }
    else if (effort) { background = pressed ? C_SELECTED_PRESS : C_ACCENT_SOFT; border = C_ACCENT; text = C_ACCENT_INK; }
    else if (disabled) { background = C_CANVAS; text = C_DISABLED_TEXT; }
    else if (pressed) background = C_BUTTON_PRESS;
    canvas.roundRect(box, current ? ButtonMetrics::selectedPageCorner : ButtonMetrics::corner, background, border);
    if (kind == Ck::PagerBtn) {
        const float cx = box.x + box.w * 0.5f, cy = box.y + box.h * 0.5f;
        const float direction = def->id == IDC_BTN_PAGE_PREV ? -1.f : 1.f;
        const Point arrow[] = {
            {cx - direction * 3, cy - 6},
            {cx + direction * 3, cy},
            {cx - direction * 3, cy + 6}
        };
        canvas.polyline(arrow, 3, disabled ? C_DISABLED_ARROW : C_MUTED, 1.5f);
    }
    else if (def->icon != ButtonIcon::None) canvas.icon(def->icon, box, text);
    else canvas.text(label, box, def->font, text, {TextAlign::Center});
}

} // namespace ui::visual
