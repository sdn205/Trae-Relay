#include "window/WindowController.h"
#include "app/Config.h"
#include <ctime>

namespace ui {
using namespace visual;

void WindowController::clearSavedHint(HWND hwnd) {
    KillTimer(hwnd, kTimerSavedHint);
    setControlText(GetDlgItem(pageStatus_, IDC_ST_MODELHINT), L"");
}

void WindowController::restoreCreditsButton(HWND hwnd) {
    KillTimer(hwnd, kTimerCreditsHint);
    setControlText(GetDlgItem(pageStatus_, IDC_BTN_CREDITS), L"刷新积分");
    InvalidateRect(GetDlgItem(pageStatus_, IDC_BTN_CREDITS), nullptr, FALSE);
}

void WindowController::tickAutoCheckin(HWND hwnd) {
    auto& cfg = Config::instance();
    time_t now = time(nullptr);
    time_t today = now / 86400;
    if (cfg.checkinEnabled) {
        struct tm local{};
        localtime_s(&local, &now);
        int nowMin = local.tm_hour * 60 + local.tm_min;
        int targetMin = cfg.checkinHour * 60 + cfg.checkinMinute;
        if (nowMin >= targetMin && lastAutoCheckinDay_.load() != today &&
            !checkinRunning_.exchange(true)) {
            checkinWorker_ = std::jthread([this, hwnd] {
                bool any = false;
                bool failed = false;
                for (auto& account : AccountPool::instance().accounts())
                {
                    any = true;
                    if (AccountPool::instance().doCheckin(*account) < 0) failed = true;
                }
                int result = !any || failed ? -1 : 1;
                PostMessageW(hwnd, WM_APP_CHECKIN_DONE, static_cast<WPARAM>(result), 0);
            });
        }
    }
}

void WindowController::tickSecond(HWND hwnd) {
    // 状态页每 3 秒刷新账号卡与今日请求计数
    bool visible = IsWindowVisible(hwnd) && !IsIconic(hwnd);
    unsigned tick = statusTick_++; // 所有页面共享真实秒数，不能只在状态页递增。
    if (visible && activePage_ == PAGE_STATUS && (tick % 3 == 0))
        refreshStatusPage(hwnd);
    // 模型目录后台刷新完成后，若正停在运行总览则热填充（保留当前选中）
    int catalogVer = ModelCatalog::instance().version();
    if (visible && activePage_ == PAGE_STATUS && pageStatus_ && catalogVer != catalogVersion_) {
        catalogVersion_ = catalogVer;
        refreshModelSettings(hwnd);
        loadModelSelection(hwnd);
    }
    // 倍率实时翻新：闲时窗口跨点/会员身份变更后，每 15s 比对一次标签，
    // 有变化才重建下拉并刷新详情（无变化不动控件，不打扰展开中的下拉）
    if (visible && activePage_ == PAGE_STATUS && pageStatus_ && (tick % 15 == 0)) {
        if (refreshModelSettings(hwnd)) loadModelSelection(hwnd);
    }
    // 使用记录页：每 2s 重读当前分页，积分落账后行内数字自动出现
    if (visible && activePage_ == PAGE_USAGE && pageUsage_ && (tick % 2 == 0))
        loadUsagePage(false);
    tickAutoCheckin(hwnd);
}

} // namespace ui
