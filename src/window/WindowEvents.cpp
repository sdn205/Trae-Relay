#include "window/WindowController.h"
#include "app/Config.h"
#include "app/Service.h"
#include "common/Crypto.h"
#include <algorithm>
#include <chrono>
#include <windowsx.h>

namespace ui {
using namespace visual;

LRESULT WindowController::handlePageMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    int page = GetDlgCtrlID(hwnd) - kPageControlBase;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paintPage(hwnd, page);
        return 0;
    case WM_PRINTCLIENT:
        renderPageContent(reinterpret_cast<HDC>(wp), hwnd, page);
        return 0;
    case WM_MOUSEWHEEL: {
        if (page != PAGE_USAGE || usageRows_.empty()) break;
        short delta = GET_WHEEL_DELTA_WPARAM(wp);
        int height = logicalHeight(hwnd);
        int visible = usageVisibleRows(height);
        int maxScroll = std::max(0, (int)usageRows_.size() - visible);
        int nu = usageScroll_ + (delta > 0 ? -2 : 2);
        nu = std::max(0, std::min(maxScroll, nu));
        if (nu != usageScroll_) {
            usageScroll_ = nu;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
        return SendMessageW(hwnd_, msg, wp, lp);
    case WM_MEASUREITEM: {
        // 控件创建期间同步发出，此时主窗句柄可能尚未赋值，直接在页面处理
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        measureControl(hwnd, measure);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return colorControl(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp),
                            brCanvas_, brSurface_);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT WindowController::handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto& cfg = Config::instance();
    switch (msg) {
    case WM_CREATE: {
        hwnd_ = hwnd;
        // 快照模式：窗口只作渲染宿主，初始化由 runSnapshot 显式执行，
        // 服务/定时器/网络线程一概不起。
        if (snapshotMode_) return 0;
        configureTitleBar(hwnd);
        rebuildFonts(hwnd);
        createNavControls(hwnd);
        createPages(hwnd);
        layoutAll(hwnd);
        setPage(PAGE_STATUS);   // 显式初始化活动页，防止启动时落到其他页
        refreshStatusPage(hwnd);
        SetTimer(hwnd, kTimerSecond, 1000, nullptr);
        // 程序开启即自动启动本地服务，不再提供手动启停入口
        {
            std::string svcError;
            if (!service::start(svcError))
                MessageBoxA(hwnd, svcError.c_str(), "服务启动失败", MB_ICONERROR);
            refreshStatusPage(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        // 模型目录后台刷新（SWR，不阻塞窗口创建）
        if (!AccountPool::instance().accounts().empty()) {
            ModelCatalog::instance().triggerRefreshAsync();
            startupWorker_ = std::jthread([](std::stop_token stop) {
                auto& pool = AccountPool::instance();
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                    auto account = pool.acquire(250);
                    if (!account) continue;
                    if (!stop.stop_requested()) pool.refreshCredits();
                    pool.release(account, true, 0);
                    break;
                }
            });
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize.x = s(hwnd, 900);
        info->ptMinTrackSize.y = s(hwnd, 680);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        rebuildFonts(hwnd);
        layoutAll(hwnd);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    }
    case WM_SIZE:
        if (pageStatus_) layoutAll(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            rc.bottom = std::min<LONG>(rc.bottom, s(hwnd, TOPBAR_H));
            Canvas canvas(hwnd, dc, rc, C_INK);
            renderChrome(canvas, hwnd);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        bounds.bottom = std::min<LONG>(bounds.bottom, s(hwnd, TOPBAR_H));
        Canvas canvas(hwnd, reinterpret_cast<HDC>(wp), bounds, C_INK);
        renderChrome(canvas, hwnd);
        return 0;
    }
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw->CtlType == ODT_COMBOBOX) drawOwnerCombo(draw);
        else {
            int id = static_cast<int>(draw->CtlID);
            const CtrlDef* def = ctrlDef(id);
            ButtonState state;
            if (def) {
                if (def->kind == Ck::NavTab) state.selected = def->page == activePage_;
                else if (def->kind == Ck::EffortBtn) state.selected = effortFromId(id) == effort_;
                else if (def->kind == Ck::Check) state.checked = isChecked(id);
                else if (def->kind == Ck::PagerSlot) {
                    int page = pagerSlotPage_[id - IDC_PAGER_SLOT0];
                    state.selected = page > 0 && page - 1 == usagePageIdx_;
                }
            }
            drawOwnerButton(draw, state);
        }
        return TRUE;
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        measureControl(hwnd, measure);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        const CtrlDef* def = ctrlDef(id);
        if (def && def->kind == Ck::NavTab && code == BN_CLICKED) setPage(def->page);
        else if (id == IDC_BTN_PAGE_PREV && code == BN_CLICKED) {
            if (usagePageIdx_ > 0) { usagePageIdx_--; usageScroll_ = 0; loadUsagePage(); }
        } else if (id == IDC_BTN_PAGE_NEXT && code == BN_CLICKED) {
            if (usageHasMore_) { usagePageIdx_++; usageScroll_ = 0; loadUsagePage(); }
        }
        else if ((id == IDC_BTN_COPYURL || id == IDC_BTN_COPY) && code == BN_CLICKED) {
            if (id == IDC_BTN_COPY && !applyApiKeyEdit(hwnd)) return 0;
            std::wstring payload = id == IDC_BTN_COPYURL ? baseUrlText() : toWide(cfg.apiKey);
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (payload.size() + 1) * sizeof(wchar_t));
                if (memory) {
                    memcpy(GlobalLock(memory), payload.c_str(), (payload.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(memory);
                    SetClipboardData(CF_UNICODETEXT, memory);
                }
                CloseClipboard();
            }
        } else if (id == IDC_BTN_RESETKEY && code == BN_CLICKED) {
            if (saveApiKeySettings(hwnd, crypto::genApiKey(), cfg.allowAnyApiKey))
                showApiKeyHint(hwnd, L"新密钥已生成");
        } else if (id == IDC_CHK_ANYKEY && code == BN_CLICKED) {
            if (applyApiKeyEdit(hwnd)) saveApiKeySettings(hwnd, cfg.apiKey, !cfg.allowAnyApiKey);
        } else if (id == IDC_ED_APIKEY && code == EN_KILLFOCUS) {
            applyApiKeyEdit(hwnd);
            invalidateField(pageStatus_, reinterpret_cast<HWND>(lp));
        } else if (id == IDC_BTN_CREDITS && code == BN_CLICKED) {
            if (creditsRunning_.exchange(true)) return 0;
            setControlText(GetDlgItem(pageStatus_, IDC_BTN_CREDITS), L"刷新中...");
            InvalidateRect(GetDlgItem(pageStatus_, IDC_BTN_CREDITS), nullptr, FALSE);
            creditsWorker_ = std::jthread([hwnd] {
                AccountPool::instance().refreshCredits();
                PostMessageW(hwnd, WM_APP_CREDITS_DONE, 0, 0);
            });
        } else if (id == IDC_BTN_CHECKIN && code == BN_CLICKED) {
            if (checkinRunning_.exchange(true)) return 0;
            setControlText(GetDlgItem(pageStatus_, IDC_BTN_CHECKIN), L"签到中...");
            InvalidateRect(GetDlgItem(pageStatus_, IDC_BTN_CHECKIN), nullptr, FALSE);
            checkinWorker_ = std::jthread([hwnd] {
                bool any = false;
                bool failed = false;
                bool newlyClaimed = false;
                for (auto& account : AccountPool::instance().accounts()) {
                    any = true;
                    int one = AccountPool::instance().doCheckin(*account);
                    if (one < 0) failed = true;
                    else if (one == 0) newlyClaimed = true;
                }
                int result = !any || failed ? -1 : newlyClaimed ? 0 : 1;
                PostMessageW(hwnd, WM_APP_CHECKIN_DONE, static_cast<WPARAM>(result), 0);
            });
        } else if (id == IDC_CB_MODEL && code == CBN_SELCHANGE) {
            loadModelSelection(hwnd);
        } else if (def && def->kind == Ck::EffortBtn && code == BN_CLICKED) {
            if (!IsWindowEnabled(reinterpret_cast<HWND>(lp))) return 0;
            effort_ = effortFromId(id);
            for (int button : { IDC_BTN_EFF0, IDC_BTN_EFF1, IDC_BTN_EFF2 })
                InvalidateRect(GetDlgItem(pageStatus_, button), nullptr, FALSE);
            applyModelSettings(hwnd);
        } else if (def && def->kind == Ck::Check && code == BN_CLICKED) {
            toggleChecked(id);
            InvalidateRect(GetDlgItem(pageHwnd(def->page), id), nullptr, FALSE);
            if (id == IDC_CHK_MAX) applyModelSettings(hwnd);
            else applySettingsInstant(hwnd);
        } else if (id == IDC_CB_LOGLEVEL && code == CBN_SELCHANGE) {
            applySettingsInstant(hwnd);
        } else if (def && def->kind == Ck::Edit && def->page == PAGE_SETTINGS &&
                   code == EN_KILLFOCUS) {
            applySettingsInstant(hwnd);
        } else if (def && (def->kind == Ck::Edit || def->kind == Ck::Combo) &&
                   (code == EN_SETFOCUS || code == EN_KILLFOCUS ||
                    code == CBN_SETFOCUS || code == CBN_KILLFOCUS)) {
            HWND page = pageHwnd(def->page);
            invalidateField(page, reinterpret_cast<HWND>(lp));
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerApiKeyHint) {
            KillTimer(hwnd, kTimerApiKeyHint);
            setControlText(GetDlgItem(pageStatus_, IDC_ST_KEYHINT), L"");
            return 0;
        }
        if (wp == kTimerSavedHint) {
            clearSavedHint(hwnd);
            return 0;
        }
        if (wp == kTimerCreditsHint) {
            restoreCreditsButton(hwnd);
            return 0;
        }
        if (wp == kTimerSecond) {
            tickSecond(hwnd);
            return 0;
        }
        return 0;
    case WM_APP_CHECKIN_DONE: {
        checkinRunning_ = false;
        int result = static_cast<int>(static_cast<INT_PTR>(wp));
        if (result >= 0)
            lastAutoCheckinDay_.store(static_cast<long long>(time(nullptr)) / 86400);
        setControlText(GetDlgItem(pageStatus_, IDC_BTN_CHECKIN),
                       result > 0 ? L"今日已签到" : result == 0 ? L"签到完成" : L"签到失败，重试");
        InvalidateRect(GetDlgItem(pageStatus_, IDC_BTN_CHECKIN), nullptr, FALSE);
        refreshStatusPage(hwnd);
        return 0;
    }
    case WM_APP_CREDITS_DONE:
        creditsRunning_ = false;
        setControlText(GetDlgItem(pageStatus_, IDC_BTN_CREDITS), L"积分已刷新");
        InvalidateRect(GetDlgItem(pageStatus_, IDC_BTN_CREDITS), nullptr, FALSE);
        SetTimer(hwnd, kTimerCreditsHint, 1500, nullptr); // 1.5s 后恢复按钮文字
        refreshStatusPage(hwnd);
        return 0;
    case WM_APP_TRAY: {
        WORD event = LOWORD(lp);
        if (event == WM_CONTEXTMENU) showTrayMenu(hwnd);
        else if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
            showMainWindow(hwnd);   // 托盘单击固定唤出主窗口
        }
        return 0;
    }
    case WM_CLOSE:
        applyApiKeyEdit(hwnd);
        if (cfg.minimizeToTrayOnClose) {
            ShowWindow(hwnd, SW_HIDE);
            if (!trayAdded_) trayAdd(hwnd);
            if (!trayTipShown_) {
                trayTipShown_ = true;
                nid_.uFlags = NIF_INFO;
                wcscpy_s(nid_.szInfo, L"Trae Relay 仍在托盘运行，右键图标可退出。");
                wcscpy_s(nid_.szInfoTitle, L"Trae Relay");
                nid_.dwInfoFlags = NIIF_INFO;
                Shell_NotifyIconW(NIM_MODIFY, &nid_);
                nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            }
        } else {
            trayRemove();
            service::stop();
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimerSecond);
        trayRemove();
        AccountPool::instance().usageFlushPending();
        service::stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace ui
