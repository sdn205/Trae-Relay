#include "window/WindowController.h"

namespace ui {
using namespace visual;

void WindowController::renderChrome(Canvas& dc, HWND hwnd) {
    int width = logicalWidth(hwnd);
    dc.fill({0, 0, width, TOPBAR_H}, C_INK);
    dc.line({0, TOPBAR_H - 2.f}, {float(width), TOPBAR_H - 2.f}, C_BUS_TRACK);
}

} // namespace ui
