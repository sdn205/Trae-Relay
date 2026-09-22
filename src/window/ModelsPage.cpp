#include "window/WindowController.h"
#include "app/Config.h"
#include <algorithm>
#include <windowsx.h>

namespace ui {
using namespace visual;

const char* WindowController::effortValue(Effort e) {
    return e == EffortLow ? "low" : e == EffortHigh ? "high" : "xhigh";
}

WindowController::Effort WindowController::effortFromId(int id) {
    return id == IDC_BTN_EFF0 ? EffortLow : id == IDC_BTN_EFF2 ? EffortXHigh : EffortHigh;
}

void WindowController::paintModelSettings(Canvas& dc, int width) {
    R card = modelSettingsCard(width);
    paintPanel(dc, card.x, card.y, card.w, card.h);
    paintGroupLabel(dc, 60, 418, L"模型设置");
    paintField(dc, pageStatus_, IDC_CB_MODEL);
    paintGroupLabel(dc, 60, 490, L"思考强度");
}

void WindowController::layoutModelSettings() {
    int width = logicalWidth(pageStatus_);
    placeField(pageStatus_, IDC_CB_MODEL);
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_BTN_EFF0), { 60, 514, 84, 34 });
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_BTN_EFF1), { 152, 514, 84, 34 });
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_BTN_EFF2), { 244, 514, 84, 34 });
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_CHK_MAX), { 360, 516, 320, 30 });
    place(pageStatus_, GetDlgItem(pageStatus_, IDC_ST_MODELHINT), { width - 360, 418, 300, 22 });
}

void WindowController::createModelControls() {
    makeControl(pageStatus_, L"",
                WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                IDC_CB_MODEL);
    makeControl(pageStatus_, L"轻", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_BTN_EFF0);
    makeControl(pageStatus_, L"高", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_BTN_EFF1);
    makeControl(pageStatus_, L"极高", WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_BTN_EFF2);
    makeControl(pageStatus_, L"启用 Max（上下文扩展至 1M）",
                WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY, IDC_CHK_MAX);
    makeControl(pageStatus_, L"", WS_VISIBLE | SS_RIGHT, IDC_ST_MODELHINT);
}

std::wstring WindowController::modelRateLabel(const RateView& v) {
    if (v.effective <= 0) return {};
    wchar_t buf[32]{};
    swprintf(buf, 32, L"%.2g", v.effective);
    std::wstring s = std::wstring(L"   ·   ") + buf + L"x";
    if (v.activityTag)
        s += std::wstring(L"（") + toWide(std::string(v.activityTag)) + L"）";
    else if (v.member && v.effective < v.original)
        s += L"（会员）";
    return s;
}

std::string WindowController::selectedModelName() {
    HWND combo = GetDlgItem(pageStatus_, IDC_CB_MODEL);
    int sel = ComboBox_GetCurSel(combo);
    if (sel < 0 || sel >= (int)modelCatalogNames_.size()) return {};
    return modelCatalogNames_[sel];
}

bool WindowController::refreshModelSettings(HWND) {
    HWND combo = GetDlgItem(pageStatus_, IDC_CB_MODEL);
    if (!combo) return false;
    auto& catalog = ModelCatalog::instance();
    if (!catalog.ready()) {
        // 目录从未拉取成功：下拉给占位提示，后台异步拉取（绝不阻塞 UI 线程）
        if (ComboBox_GetCount(combo) == 0) {
            setControlText(GetDlgItem(pageStatus_, IDC_ST_MODELHINT), L"模型目录加载中…");
            enableControl(GetDlgItem(pageStatus_, IDC_CHK_MAX), false);
        }
        return false;
    }
    std::string current = selectedModelName();
    auto models = catalog.cached();
    // 实时拼好全部标签（会员身份/闲时窗口/目录任一变化都要翻新），与现有下拉逐项
    // 比对，完全一致就不动控件——不打扰展开中的下拉，也让倍率跨过时间窗后自动刷新。
    int identity = AccountPool::instance().payIdentityForDisplay();
    std::vector<std::wstring> labels;
    labels.reserve(models.size());
    for (auto& model : models)
        labels.push_back(toWide(model.configName) + modelRateLabel(modelRateView(model, identity)));
    bool same = ComboBox_GetCount(combo) == (int)labels.size();
    if (same) {
        wchar_t buf[512]{};
        for (int i = 0; i < (int)labels.size(); ++i) {
            ComboBox_GetLBText(combo, i, buf);
            if (labels[i] != buf) { same = false; break; }
        }
    }
    if (same) return false;
    // 替换条目时抑制 Reset/Add/Select 的中间空列表绘制；保持隐藏页的可见位。
    bool redrawCombo = (GetWindowLongPtrW(combo, GWL_STYLE) & WS_VISIBLE) != 0;
    if (redrawCombo) SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
    modelCatalogNames_.clear();
    modelCatalogCaps_.clear();
    ComboBox_ResetContent(combo);
    for (size_t i = 0; i < models.size(); ++i) {
        modelCatalogNames_.push_back(models[i].configName);
        modelCatalogCaps_.push_back(models[i]);
        ComboBox_AddString(combo, labels[i].c_str());
    }
    int restore = -1;
    for (size_t i = 0; i < modelCatalogNames_.size(); ++i) {
        if (_stricmp(modelCatalogNames_[i].c_str(), current.c_str()) == 0) { restore = (int)i; break; }
    }
    if (restore < 0 && !modelCatalogNames_.empty()) restore = 0;
    if (restore >= 0) ComboBox_SetCurSel(combo, restore);
    if (redrawCombo) {
        SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(combo, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    return true;
}

void WindowController::applyModelSettings(HWND) {
    std::string modelName = selectedModelName();
    if (modelName.empty()) return;
    auto& cfg = Config::instance();
    std::string effort = effortValue(effort_);
    bool maxEnabled = isChecked(IDC_CHK_MAX);
    ModelConfig model;
    const ModelConfig* old = cfg.modelConfig(modelName);
    if (old) model = *old;
    model.present = true;
    model.name = modelName;
    model.reasoningEffort = effort;
    model.isMaxMode = maxEnabled ? 1 : 0;
    // 窗口档位不再手选：勾选 Max 时由后端取该模型档案的最大档（兜底 1M）
    model.maxContextWindow = 0;
    cfg.upsertModel(model);
    std::string error;
    if (!cfg.save(error)) {
        setControlText(GetDlgItem(pageStatus_, IDC_ST_MODELHINT), toWide(error).c_str());
    } else {
        setControlText(GetDlgItem(pageStatus_, IDC_ST_MODELHINT), L"");
    }
    // 模型覆盖表已变：重推 core 设置快照
    settings::set(makeCoreSettings(cfg));
    // 不再 invalidate 强制同步重拉：倍率/能力由后台 SWR 每 5 分钟自动刷新
}

bool WindowController::supportsEffort(const ModelCaps& caps, const std::string& effort) {
    if (caps.effortOptions.empty() && caps.effortOptionsExt.empty()) return true;
    return std::find(caps.effortOptions.begin(), caps.effortOptions.end(), effort) != caps.effortOptions.end() ||
           std::find(caps.effortOptionsExt.begin(), caps.effortOptionsExt.end(), effort) != caps.effortOptionsExt.end();
}

void WindowController::loadModelSelection(HWND) {
    HWND combo = GetDlgItem(pageStatus_, IDC_CB_MODEL);
    int sel = ComboBox_GetCurSel(combo);
    std::string modelName = selectedModelName();
    if (modelName.empty()) return;
    // 切换模型时清掉上一个模型的提示
    setControlText(GetDlgItem(pageStatus_, IDC_ST_MODELHINT), L"");
    auto& cfg = Config::instance();
    const ModelConfig* model = cfg.modelConfig(modelName);
    std::string effort = model && !model->reasoningEffort.empty() ? model->reasoningEffort
                                                                  : cfg.defaultReasoningEffort;
    effort_ = effort == "low" ? EffortLow
               : (effort == "xhigh" || effort == "extra_high") ? EffortXHigh
                                                               : EffortHigh;
    for (int id : { IDC_BTN_EFF0, IDC_BTN_EFF1, IDC_BTN_EFF2 })
        InvalidateRect(GetDlgItem(pageStatus_, id), nullptr, FALSE);
    setChecked(IDC_CHK_MAX, model && model->isMaxMode);
    InvalidateRect(GetDlgItem(pageStatus_, IDC_CHK_MAX), nullptr, FALSE);
    // 能力档案直接读内存目录（不发网络、不占账号槽，切页零卡顿）
    if (sel < 0 || sel >= (int)modelCatalogCaps_.size()) {
        setControlText(GetDlgItem(pageStatus_, IDC_ST_MODELHINT), L"模型目录加载中…");
        enableControl(GetDlgItem(pageStatus_, IDC_CHK_MAX), false);
        enableControl(GetDlgItem(pageStatus_, IDC_BTN_EFF0), true);
        enableControl(GetDlgItem(pageStatus_, IDC_BTN_EFF1), true);
        enableControl(GetDlgItem(pageStatus_, IDC_BTN_EFF2), true);
        return;
    }
    ModelCaps caps = modelCatalogCaps_[sel];
    enableControl(GetDlgItem(pageStatus_, IDC_CHK_MAX), caps.maxMode);
    if (!caps.maxMode) setChecked(IDC_CHK_MAX, false);
    enableControl(GetDlgItem(pageStatus_, IDC_BTN_EFF0), supportsEffort(caps, "low"));
    enableControl(GetDlgItem(pageStatus_, IDC_BTN_EFF1), supportsEffort(caps, "high"));
    enableControl(GetDlgItem(pageStatus_, IDC_BTN_EFF2), supportsEffort(caps, "xhigh"));
}

} // namespace ui
