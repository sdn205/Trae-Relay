#pragma once
#include "ui/UiStyle.h"
#include <algorithm>

namespace ui::visual {
// ===== 几何单一事实源 =====
// 字段控件在其自绘外框内的矩形：EDIT 内缩 (10,4) 高 22，COMBO 内缩 (1,1) 高为下拉整体高。
// paint* 画外框与 layout* 放控件读同一份外框几何，不再各自硬编码、靠魔法偏移对齐。
R editCtrl(R box);
R comboCtrl(R box, int dropH);

// 使用记录行几何（行区顶部/行高/可见行数）：表格绘制、TOKEN 列、滚轮步进三处同源
constexpr int kUsageTop = 126;
constexpr int kUsageRowH = 48;
int usageVisibleRows(int height);
// 分页条（‹ 槽×7 ›）纵坐标：装载布局与"第 x/y 页"信息文字同源
int pagerY(int height);
struct UsageCols {
    int x[5];
    int w[5];
};
UsageCols usageColumns(int width);

// 运行总览三张卡（账号卡同时用于局部刷新）
R endpointCard(int width);
R accountCard(int width);
R modelSettingsCard(int width);
// 状态页两个输入框外框（paint 画框与 layout 放控件同源）
R baseUrlBox(int width);
R apiKeyBox(int width);
// 运行总览模型设置下拉外框
R modelComboBox(int width);
// 设置页输入框外框
R portBox(int = 0);
R hourBox(int = 0);
R minuteBox(int = 0);
R logLevelBox(int = 0);


} // namespace ui::visual
