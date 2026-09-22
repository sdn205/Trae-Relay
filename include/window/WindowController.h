#pragma once
#include "ui/UiControls.h"
#include "accounts/AccountPool.h"
#include "upstream/ModelCatalog.h"
#include <atomic>
#include <map>
#include <shellapi.h>
#include <thread>

namespace ui {
class WindowController final {
public:
    WindowController();
    ~WindowController();
    WindowController(const WindowController&) = delete;
    WindowController& operator=(const WindowController&) = delete;
    int runGui(bool startMinimized);
    int runSnapshot(const std::string& dirUtf8);

private:
    static constexpr int TOPBAR_H = 64;
    static constexpr int kPageControlBase = 2000;
    static constexpr int IDI_APPICON = 101;
    static constexpr UINT WM_APP_TRAY = WM_APP + 1;
    static constexpr UINT WM_APP_CHECKIN_DONE = WM_APP + 2;
    static constexpr UINT WM_APP_CREDITS_DONE = WM_APP + 3;
    enum Timers { kTimerSecond = 1, kTimerSavedHint, kTimerCreditsHint = 4, kTimerApiKeyHint };
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK pageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool snapshotMode_ = false;
    
    HWND hwnd_ = nullptr;
    HWND pageStatus_ = nullptr;
    HWND pageSettings_ = nullptr;
    HWND pageUsage_ = nullptr;
    HWND navStatus_ = nullptr;
    HWND navSettings_ = nullptr;
    HWND navUsage_ = nullptr;
    HWND endpointTooltips_ = nullptr;
    int activePage_ = visual::PAGE_STATUS;
    // 使用记录分页状态（每页 20 条，最多保留最近 100 条）
    int usagePageIdx_ = 0;
    bool usageHasMore_ = false;
    int usageScroll_ = 0; // 滚轮与右侧滑块共用页内位置，0 = 最新一条在顶
    bool usageDragging_ = false;
    int usageDragOffset_ = 0;
    bool usageEmpty_ = false;
    int usageTotalPages_ = 1;
    int pagerSlotPage_[7] = { 0 }; // 各页码槽对应页（0=空槽）；1-based
    int pagerX_ = 0;               // 分页条左缘（逻辑坐标），信息文字贴其左侧
    std::vector<UsageRecord> usageRows_; // 当前行数据（自绘列对齐用）
    // 思考强度三档状态（不再用控件 ID 充当状态值）
    enum Effort { EffortLow, EffortHigh, EffortXHigh };
    Effort effort_ = EffortHigh;
    // BS_OWNERDRAW 复选框的选中态必须自行维护（BM_SETCHECK 对非 checkbox 风格无效）
    std::map<int, bool> checkStates_;
    // 模型下拉按 index 对齐的真实 configName 与能力档案（下拉显示文本追加倍率）
    std::vector<std::string> modelCatalogNames_;
    std::vector<ModelCaps> modelCatalogCaps_;
    int catalogVersion_ = -1; // 已填充到 UI 的模型目录版本（ModelCatalog::version）
    
    unsigned int statusTick_ = 0;
    
    
    HBRUSH brCanvas_ = nullptr;
    HBRUSH brSurface_ = nullptr;
    NOTIFYICONDATAW nid_{};
    bool trayAdded_ = false;
    bool trayTipShown_ = false;
    std::atomic<bool> checkinRunning_{ false };
    std::atomic<bool> creditsRunning_{ false };
    std::atomic<long long> lastAutoCheckinDay_{ -1 };
    
    struct UsagePagerLayout {
        HWND window = nullptr;
        int page = -1, pages = -1, width = -1, height = -1;
        UINT dpiValue = 0;
        bool empty = false, hasMore = false;
        bool operator==(const UsagePagerLayout&) const = default;
    };
    struct CardState {
        std::wstring nickname, credits;
        int state = -1;
        std::wstring creditsConsumed;
        long long tokens = 0;
        bool operator==(const CardState&) const = default;
    };
    UsagePagerLayout previousPager_;
    CardState previousCard_;
    HWND previousCardWindow_ = nullptr;
    std::jthread startupWorker_, creditsWorker_, checkinWorker_;
    bool applyingSettings_ = false;
    bool applyingApiKey_ = false;
    HWND pageHwnd(int page);
    bool isChecked(int id);
    void setChecked(int id, bool value);
    void toggleChecked(int id);
    void rebuildFonts(HWND hwnd);
    void configureTitleBar(HWND hwnd);
    void registerClasses();
    LRESULT handlePageMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void renderPageContent(HDC target, HWND hwnd, int page);
    void paintPage(HWND hwnd, int page);
    void layoutAll(HWND hwnd);
    void createNavControls(HWND hwnd);
    void createPages(HWND hwnd);
    void setPage(int page);
    void paintStatusPage(visual::Canvas& dc, int width, int);
    void layoutStatusPage();
    std::wstring baseUrlText();
    void createStatusControls();
    void refreshStatusPage(HWND);
    bool applyApiKeyEdit(HWND hwnd);
    bool saveApiKeySettings(HWND hwnd, const std::string& key, bool allowAny);
    void showApiKeyHint(HWND hwnd, const std::wstring& text);
    const char* effortValue(Effort e);
    Effort effortFromId(int id);
    void paintModelSettings(visual::Canvas& dc, int width);
    void layoutModelSettings();
    void createModelControls();
    std::wstring modelRateLabel(const RateView& v);
    std::string selectedModelName();
    bool refreshModelSettings(HWND);
    void applyModelSettings(HWND hwnd);
    bool supportsEffort(const ModelCaps& caps, const std::string& effort);
    void loadModelSelection(HWND);
    void paintSettingsPage(visual::Canvas& dc, int width, int);
    void layoutSettingsPage();
    void createSettingsControls();
    void applySettingsInstant(HWND hwnd);
    void usagePagerItems(int cur, int total, int* slots);
    void layoutUsagePager(const UsagePagerLayout& layout);
    void loadUsagePage(bool forcePaint = true);
    void scrollUsageTo(int position);
    void layoutUsagePage();
    void createUsageControls();
    void renderChrome(visual::Canvas& dc, HWND hwnd);
    void clearSavedHint(HWND hwnd);
    void restoreCreditsButton(HWND hwnd);
    void tickAutoCheckin(HWND hwnd);
    void tickSecond(HWND hwnd);
    HICON appIcon();
    void trayAdd(HWND hwnd);
    void trayRemove();
    void showMainWindow(HWND hwnd);
    void showTrayMenu(HWND hwnd);
    const wchar_t* pageName(int page);
    void printChildControls(HWND page, HDC dc, int orgX, int orgY);
};
} // namespace ui
