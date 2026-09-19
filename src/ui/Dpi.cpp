#include "ui/Dpi.h"
#include <shellscalingapi.h>

namespace dpi {
UINT forWindow(HWND hwnd) { return GetDpiForWindow(hwnd); }
int scale(HWND hwnd, int logical) { return MulDiv(logical, static_cast<int>(forWindow(hwnd)), 96); }
UINT forPrimaryMonitor() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    POINT center{(work.left + work.right) / 2, (work.top + work.bottom) / 2};
    UINT x = 96, y = 96;
    GetDpiForMonitor(MonitorFromPoint(center, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &x, &y);
    return y;
}
}
