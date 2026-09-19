#pragma once
#include <windows.h>
namespace dpi {
UINT forWindow(HWND hwnd);
int scale(HWND hwnd, int logical);
UINT forPrimaryMonitor();
}
