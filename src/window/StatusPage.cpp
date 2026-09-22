#include "window/WindowController.h"
#include "app/Config.h"
#include "common/Stats.h"
#include <ctime>

namespace ui {
using namespace visual;

namespace {
std::wstring formatCreditsConsumed(double credits) {
    wchar_t text[64]{};
    swprintf(text, 64, L"%.2f", credits);
    return text;
}

std::wstring formatTokens(long long tokens) {
    double value = static_cast<double>(tokens);
    const wchar_t* units[] = { L"", L"K", L"M", L"B" };
    int unit = 0;
    // Promote rounded values instead of displaying 1000.0K.
    while (unit < 3 && value >= 999.95) {
        value /= 1000.0;
        ++unit;
    }
    wchar_t text[64]{};
    swprintf(text, 64, L"%.1f%s", value, units[unit]);
    return text;
}

void updateApiKeyControls(HWND page, bool allowAny) {
    for (int id : { IDC_ED_APIKEY, IDC_BTN_COPY, IDC_BTN_RESETKEY }) {
        HWND control = GetDlgItem(page, id);
        enableControl(control, !allowAny);
        InvalidateRect(control, nullptr, FALSE);
    }
    invalidateField(page, GetDlgItem(page, IDC_ED_APIKEY));
}
} // namespace

void WindowController::paintStatusPage(Canvas& dc, int width, int) {
    paintPageHeader(dc, width, L"运行总览");

    // —— 当前账号：状态 / 积分 / 今日 Token / 今日积分消耗 ——
    R acct = accountCard(width);
    paintPanel(dc, acct.x, acct.y, acct.w, acct.h);
    paintGroupLabel(dc, 60, 94, L"当前账号");
    auto& pool = AccountPool::instance();
    if (pool.accounts().empty()) {
        drawText(dc, L"未发现已登录的 Trae 账号",
                 { 60, 124, width - 120, 30 }, FMetric, C_TEXT,
                 DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        drawText(dc, L"请先在 Trae 客户端完成登录，再回到本页查看",
                 { 60, 172, width - 120, 26 }, FSmall, C_MUTED,
                 DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    } else {
        auto& account = pool.accounts().front();
        std::wstring nickname = toWide(account->nickname);
        if (nickname.empty()) nickname = L"未命名账号";
        drawText(dc, nickname, { 60, 120, width - 360, 32 }, FMetricSemibold, C_TEXT,
                 DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        // 四列：小标签在上，读数在下
        const int columnWidth = (width - 120) / 4;
        const int valueWidth = columnWidth - 20;
        const int creditsX = 60 + columnWidth;
        const int tokensX = 60 + columnWidth * 2;
        const int consumedX = 60 + columnWidth * 3;
        const auto today = stats::usageToday();
        paintGroupLabel(dc, 60, 164, L"状态");
        paintGroupLabel(dc, creditsX, 164, L"积分");
        paintGroupLabel(dc, tokensX, 164, L"今日 Token");
        paintGroupLabel(dc, consumedX, 164, L"今日积分消耗");
        const bool queued = account->queued.load() > 0;
        const bool active = account->active.load() > 0;
        const wchar_t* stateText = queued ? L"排队中" : active ? L"请求中" : L"正常";
        COLORREF stateColor = queued ? C_QUEUED : active ? C_REQUEST : C_ACCENT;
        dc.ellipse({62, 194, 8, 8}, stateColor);
        drawText(dc, stateText, { 78, 182, valueWidth - 18, 32 }, FBody, C_TEXT,
                 DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        double amount = account->credits.load();
        std::wstring credits = L"--";
        if (amount >= 0) {
            wchar_t buf[64]{};
            swprintf(buf, 64, L"%.2f", amount);
            credits = buf;
        }
        drawText(dc, credits, { creditsX, 180, valueWidth, 32 }, FMetricSemibold, C_TEXT,
                 DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        drawText(dc, formatTokens(today.tokens),
                 { tokensX, 180, valueWidth, 32 }, FMetricSemibold, C_TEXT,
                 DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        drawText(dc, formatCreditsConsumed(today.creditsConsumed),
                 { consumedX, 180, valueWidth, 32 }, FMetricSemibold, C_TEXT,
                 DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    // —— 本地端点与密钥 ——
    R card = endpointCard(width);
    paintPanel(dc, card.x, card.y, card.w, card.h);
    paintGroupLabel(dc, 60, 258, L"本地端点");
    drawText(dc, L"端点", { 60, 284, 44, 32 }, FBody, C_MUTED,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    drawText(dc, L"密钥", { 60, 330, 44, 32 }, FBody, C_MUTED,
             DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    paintField(dc, pageStatus_, IDC_ED_BASEURL);
    paintField(dc, pageStatus_, IDC_ED_APIKEY);
    paintModelSettings(dc, width);
}

void WindowController::layoutStatusPage() {
    int width = logicalWidth(pageStatus_);
    // 端点卡：字段外框来自几何源，控件按 EDIT 规则内缩、垂直居中
    placeField(pageStatus_, IDC_ED_BASEURL);
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_BTN_COPYURL), { width - 92, 284, 32, 32 });
    placeField(pageStatus_, IDC_ED_APIKEY);
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_BTN_COPY), { width - 224, 330, 32, 32 });
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_BTN_RESETKEY), { width - 184, 330, 32, 32 });
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_CHK_ANYKEY), { width - 140, 332, 80, 28 });
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_ST_KEYHINT), { 160, 257, width - 370, 22 });
    // 账号卡：操作按钮在卡右上
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_BTN_CREDITS), { width - 262, 94, 100, 32 });
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_BTN_CHECKIN), { width - 156, 94, 96, 32 });
    layoutModelSettings();
}

std::wstring WindowController::baseUrlText() {
    auto& cfg = Config::instance();
    return L"http://" + toWide(cfg.serviceHost) + L":" + std::to_wstring(cfg.servicePort);
}

void WindowController::createStatusControls() {
    auto& cfg = Config::instance();
    makeControl(pageStatus_, L"刷新积分", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_BTN_CREDITS);
    makeControl(pageStatus_, L"立即签到", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_BTN_CHECKIN);
    makeControl(pageStatus_, L"任意 Key", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_CHK_ANYKEY);
    setChecked(IDC_CHK_ANYKEY, cfg.allowAnyApiKey);
    makeControl(pageStatus_, L"", WS_VISIBLE | SS_LEFT, IDC_ST_KEYHINT);
    makeControl(pageStatus_, L"",
                WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL, IDC_ED_BASEURL);
    setControlText(GetDlgItem(pageStatus_, IDC_ED_BASEURL), baseUrlText().c_str());
    makeControl(pageStatus_, L"复制地址", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_BTN_COPYURL);
    makeControl(pageStatus_, L"",
                WS_VISIBLE | ES_AUTOHSCROLL, IDC_ED_APIKEY);
    setControlText(GetDlgItem(pageStatus_, IDC_ED_APIKEY), toWide(cfg.apiKey).c_str());
    makeControl(pageStatus_, L"复制密钥", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_BTN_COPY);
    makeControl(pageStatus_, L"重新生成", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_BTN_RESETKEY);
    updateApiKeyControls(pageStatus_, cfg.allowAnyApiKey);
    endpointTooltips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (endpointTooltips_) {
        SetWindowPos(endpointTooltips_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(endpointTooltips_, TTM_SETMAXTIPWIDTH, 0, s(hwnd_, 360));
        const auto addTip = [&](int id, const wchar_t* text) {
            TOOLINFOW tool{sizeof(tool)};
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = pageStatus_;
            tool.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(pageStatus_, id));
            tool.lpszText = const_cast<wchar_t*>(text);
            SendMessageW(endpointTooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        };
        addTip(IDC_BTN_COPYURL, L"复制地址");
        addTip(IDC_BTN_COPY, L"复制密钥");
        addTip(IDC_BTN_RESETKEY, L"重新生成密钥");
        addTip(IDC_ED_APIKEY, L"可自定义密钥，移开焦点后自动保存并生效");
        addTip(IDC_CHK_ANYKEY, L"开启后不校验请求密钥，可使用任意 Key 或不填写；关闭后校验已保存的密钥");
    }
    createModelControls();
}

void WindowController::showApiKeyHint(HWND hwnd, const std::wstring& text) {
    setControlText(GetDlgItem(pageStatus_, IDC_ST_KEYHINT), text.c_str());
    SetTimer(hwnd, kTimerApiKeyHint, 4000, nullptr);
}

bool WindowController::saveApiKeySettings(HWND hwnd, const std::string& key, bool allowAny) {
    if (applyingApiKey_) return false;
    applyingApiKey_ = true;
    struct SavingScope { bool& active; ~SavingScope() { active = false; } } saving{applyingApiKey_};
    auto& cfg = Config::instance();
    if (key == cfg.apiKey && allowAny == cfg.allowAnyApiKey) return true;
    const std::string previousKey = cfg.apiKey;
    const bool previousAny = cfg.allowAnyApiKey;
    cfg.apiKey = key;
    cfg.allowAnyApiKey = allowAny;
    std::string error;
    const bool saved = cfg.save(error);
    if (saved) settings::set(makeCoreSettings(cfg));
    else {
        cfg.apiKey = previousKey;
        cfg.allowAnyApiKey = previousAny;
    }
    setControlText(GetDlgItem(pageStatus_, IDC_ED_APIKEY), toWide(cfg.apiKey).c_str());
    setChecked(IDC_CHK_ANYKEY, cfg.allowAnyApiKey);
    InvalidateRect(GetDlgItem(pageStatus_, IDC_CHK_ANYKEY), nullptr, FALSE);
    updateApiKeyControls(pageStatus_, cfg.allowAnyApiKey);
    if (saved) {
        KillTimer(hwnd, kTimerApiKeyHint);
        setControlText(GetDlgItem(pageStatus_, IDC_ST_KEYHINT), L"");
    } else showApiKeyHint(hwnd, L"保存失败：" + toWide(error));
    return saved;
}

bool WindowController::applyApiKeyEdit(HWND hwnd) {
    if (!pageStatus_ || applyingApiKey_) return false;
    HWND edit = GetDlgItem(pageStatus_, IDC_ED_APIKEY);
    std::wstring value(GetWindowTextLengthW(edit) + 1, L'\0');
    GetWindowTextW(edit, value.data(), static_cast<int>(value.size()));
    value.resize(wcslen(value.c_str()));
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        setControlText(edit, toWide(Config::instance().apiKey).c_str());
        showApiKeyHint(hwnd, L"密钥不能为空；不校验密钥请勾选“任意 Key”");
        return false;
    }
    value = value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
    const bool saved = saveApiKeySettings(hwnd, toUtf8(value), Config::instance().allowAnyApiKey);
    if (saved) setControlText(edit, value.c_str());
    return saved;
}

void WindowController::refreshStatusPage(HWND) {
    if (!pageStatus_) return;
    HWND urlEdit = GetDlgItem(pageStatus_, IDC_ED_BASEURL);
    if (urlEdit) setControlText(urlEdit, baseUrlText().c_str());
    bool hasAccount = !AccountPool::instance().accounts().empty();
    enableControl(GetDlgItem(pageStatus_, IDC_BTN_CREDITS), hasAccount);
    enableControl(GetDlgItem(pageStatus_, IDC_BTN_CHECKIN),
                 hasAccount && !checkinRunning_.load());
    // 比较账号卡真正显示的值；余额小数位、请求状态及跨日请求数均纳入。
    CardState current;
    if (hasAccount) {
        const auto& account = AccountPool::instance().accounts().front();
        current.nickname = toWide(account->nickname);
        current.state = account->queued.load() > 0 ? 4 : account->active.load() > 0 ? 2 : 3;
        double amount = account->credits.load();
        wchar_t amountText[64]{};
        if (amount >= 0) swprintf(amountText, 64, L"%.2f", amount);
        current.credits = amount >= 0 ? amountText : L"--";
        const auto today = stats::usageToday();
        current.creditsConsumed = formatCreditsConsumed(today.creditsConsumed);
        current.tokens = today.tokens;
    }
    bool changed = previousCardWindow_ != pageStatus_ || current != previousCard_;
    previousCard_ = std::move(current);
    previousCardWindow_ = pageStatus_;
    if (changed && activePage_ == PAGE_STATUS && !IsIconic(hwnd_)) {
        int width = logicalWidth(pageStatus_);
        // 账号卡外扩 8px 局部失效，不整页重绘
        R acct = accountCard(width);
        RECT card = physRect(pageStatus_, acct.x - 40, acct.y - 8, acct.w + 80, acct.h + 16);
        InvalidateRect(pageStatus_, &card, FALSE);
    }
}

} // namespace ui
