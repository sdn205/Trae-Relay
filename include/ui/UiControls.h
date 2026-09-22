#pragma once
#include "ui/UiDrawing.h"
#include "ui/UiLayout.h"
#include <commctrl.h>

namespace ui::visual {
// Main navigation
inline constexpr int IDC_NAV_STATUS = 1001;
inline constexpr int IDC_NAV_SETTINGS = 1003;
inline constexpr int IDC_NAV_USAGE = 1004;
// Overview
inline constexpr int IDC_ED_APIKEY = 1101;
inline constexpr int IDC_BTN_COPY = 1102;
inline constexpr int IDC_BTN_RESETKEY = 1103;
inline constexpr int IDC_BTN_CREDITS = 1106;
inline constexpr int IDC_BTN_CHECKIN = 1107;
inline constexpr int IDC_ED_BASEURL = 1108;
inline constexpr int IDC_BTN_COPYURL = 1109;
inline constexpr int IDC_CHK_ANYKEY = 1110;
inline constexpr int IDC_ST_KEYHINT = 1111;
// Usage pagination
inline constexpr int IDC_BTN_PAGE_PREV = 1401;
inline constexpr int IDC_BTN_PAGE_NEXT = 1402;
inline constexpr int IDC_PAGER_SLOT0 = 1410; // 页码槽 ×7（1410..1416）
// Model settings in overview
inline constexpr int IDC_CB_MODEL = 1200;
inline constexpr int IDC_BTN_EFF0 = 1201;
inline constexpr int IDC_BTN_EFF1 = 1202;
inline constexpr int IDC_BTN_EFF2 = 1203;
inline constexpr int IDC_CHK_MAX = 1205;
inline constexpr int IDC_ST_MODELHINT = 1211;
// Preferences
inline constexpr int IDC_ED_PORT = 1300;
inline constexpr int IDC_CHK_LAN = 1301;
inline constexpr int IDC_CHK_AUTOSTART = 1302;
inline constexpr int IDC_CHK_STARTMIN = 1303;
inline constexpr int IDC_CHK_MINCLOSE = 1304;
inline constexpr int IDC_CHK_CHECKIN = 1306;
inline constexpr int IDC_ED_HOUR = 1307;
inline constexpr int IDC_CHK_LOGGING = 1308;
inline constexpr int IDC_ED_MINUTE = 1311;
enum PageIndex { PAGE_STATUS = 0, PAGE_USAGE, PAGE_SETTINGS };

// ===== 控件描述表 =====
// 每个控件在此登记身份、字体、所属页及字段外框。
// 控件创建、字体、上色、焦点与绘制均读同一描述。业务状态由主窗口提供。
// 新增控件不再需要同步散落各处的 if 清单。
enum class Ck {
    Edit,         // 单行输入（surface 底、聚焦描边、焦点重绘）
    Combo,        // 下拉（去主题 + 子类自绘显示区、焦点重绘）
    Check,        // 自绘复选框（选中态由调用方提供）
    Btn,          // 通用按钮
    PrimaryBtn,   // 主操作按钮（Signal 底白字）
    EffortBtn,    // 思考强度三档按钮（互斥选中）
    NavTab,       // 顶带导航 tab（自绘用 FTab）
    PagerSlot,    // 使用记录分页条页码槽（当前页强调底色）
    PagerBtn,     // 分页条 ‹ › 箭头
    Static,       // 卡内说明文字（muted + 面板白）
    StaticAccent, // 强调说明文字（accent + 画布）
};
struct CtrlDef {
    int id;
    Ck kind;
    FontId font; // 原生字体与自绘文字共用此档位
    int page;    // 所属页 PageIndex；nav 为点击目标页
    R (*fieldBox)(int width) = nullptr;
    ButtonIcon icon = ButtonIcon::None;
};


const CtrlDef* ctrlDef(int id);
FontId controlFontId(int id);
void setControlText(HWND control, const wchar_t* text);
void enableControl(HWND control, bool enabled);
void applyControlFonts(HWND hwnd);
HWND makeControl(HWND parent, const wchar_t* text, DWORD style, int id);
void paintField(Canvas& canvas, HWND page, int id);
void placeField(HWND page, int id);
void invalidateField(HWND page, HWND control);
void measureControl(HWND parent, MEASUREITEMSTRUCT* measure);
LRESULT colorControl(UINT message, HDC dc, HWND control, HBRUSH canvas, HBRUSH surface);
void drawOwnerCombo(const DRAWITEMSTRUCT* item);
// 纯视觉状态：渲染器不读取页码、配置、服务或全局控件状态。
struct ButtonState { bool selected = false; bool checked = false; };
void drawOwnerButton(const DRAWITEMSTRUCT* item, ButtonState state);
} // namespace ui::visual
