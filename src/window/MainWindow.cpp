#include "window/MainWindow.h"
#include "window/WindowController.h"

namespace ui {
int runGui(bool startMinimized) {
    WindowController window;
    return window.runGui(startMinimized);
}
int runSnapshot(const std::string& directory) {
    WindowController window;
    return window.runSnapshot(directory);
}
} // namespace ui
