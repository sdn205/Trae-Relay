// ModelCatalog.cpp - 模型配置表缓存（get_detail_param 主目录）
#include "upstream/ModelCatalog.h"
#include "accounts/AccountPool.h"
#include "common/Log.h"
#include "common/Settings.h"
#include "upstream/Upstream.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <thread>

static std::string jstr(const Json& j, std::initializer_list<const char*> keys, const std::string& def = "") {
    for (auto k : keys) {
        const Json* v = j.find(k);
        if (v && v->isString()) return v->asString();
        if (v && v->isNumber()) return std::to_string((long long)v->asDouble());
    }
    return def;
}
static long long jint(const Json& j, std::initializer_list<const char*> keys, long long def) {
    for (auto k : keys) {
        const Json* v = j.find(k);
        if (v && v->isNumber()) return v->asInt(def);
        if (v && v->isString()) {
            char* e = nullptr;
            long long r = _strtoi64(v->asString().c_str(), &e, 10);
            if (e && *e == '\0' && e != v->asString().c_str()) return r;
        }
    }
    return def;
}

static std::string normalizeEffort(std::string value) {
    for (auto& c : value) c = (char)tolower((unsigned char)c);
    size_t a = value.find_first_not_of(" \t");
    size_t b = value.find_last_not_of(" \t");
    if (a == std::string::npos) return {};
    value = value.substr(a, b - a + 1);
    // Trae 当前接口对内置模型返回 light，归一到客户端协议的 low；
    // chat_v3 目录用 extra_high 表示极高，归一到内部 xhigh。
    if (value == "light") return "low";
    if (value == "extra_high" || value == "x-high" || value == "x_high" ||
        value == "max" || value == "ultra")
        return "xhigh";
    return value;
}

static bool isInternalModelName(const std::string& name) {
    std::string lower = name;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    // custom_* 是用户自定义连接，不属于 Trae 内置模型目录。
    if (lower.rfind("custom_", 0) == 0) return true;
    // Agent/Code 目录还会返回自动代理、搜索代理和内部审查节点；这些
    // 不是用户可选的主模型，不能混进 /v1/models。
    if (lower.rfind("search_agent", 0) == 0 ||
        lower.rfind("browser_use", 0) == 0 ||
        lower.rfind("computer_use", 0) == 0 ||
        lower.rfind("file_search", 0) == 0 ||
        lower.rfind("explore_", 0) == 0 ||
        lower.size() > 5 && lower.compare(lower.size() - 5, 5, "-auto") == 0)
        return true;
    if (lower == "doubao-for-auto" || lower == "glm-4.7" ||
        lower == "glm-4.6" || lower == "minimax-m2" ||
        lower == "minimax-m2.1" || lower == "kimi-k2-0905") return true;
    for (auto& suffix : { std::string("summary"), std::string("fast_apply"),
                          std::string("fast_apply_new"), std::string("title_generation"),
                          std::string("input_optimization"), std::string("context_selection") }) {
        if (lower == suffix) return true;
    }
    return false;
}

static bool jsonBool(const Json& j, const char* key, bool def = false) {
    const Json* v = j.find(key);
    return v && v->isBool() ? v->asBool(def) : def;
}

// 视觉能力探测。Trae 目录没有稳定公开的字段名，这里只认明确信号：
// support_vision / vision_enabled / multimodal 等布尔键，或能力数组里出现
// vision / image。任何明确信号都没有时，调用方按 true 透传（上游不支持会
// 明确报错），避免把支持图片的模型误判成纯文本。
static void detectVision(const Json& m, bool& sawSignal, bool& vision) {
    static const char* kBoolKeys[] = {
        "support_vision", "vision_support", "vision_enabled", "support_image",
        "support_image_input", "image_input_enabled", "multimodal", "is_multimodal",
        "support_multimodal", "vision"
    };
    if (m.isObject()) {
        for (auto k : kBoolKeys) {
            const Json* v = m.find(k);
            if (!v) continue;
            sawSignal = true;
            if (v->isBool()) vision = vision || v->asBool();
            else if (v->isString()) {
                std::string s = v->asString();
                for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
                vision = vision || s == "true" || s == "1" || s == "yes";
            }
        }
        // capabilities / modalities / tags 等数组里出现 vision / image / multimodal
        static const char* kArrKeys[] = { "capabilities", "modalities", "tags", "features_list" };
        for (auto k : kArrKeys) {
            const Json* a = m.find(k);
            if (!a || !a->isArray()) continue;
            for (size_t i = 0; i < a->size(); ++i) {
                if (!a->at(i).isString()) continue;
                std::string s = a->at(i).asString();
                for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
                if (s.find("vision") != std::string::npos || s.find("image") != std::string::npos ||
                    s.find("multimodal") != std::string::npos) {
                    sawSignal = true;
                    vision = true;
                }
            }
        }
    }
}

static void capsFromModelJson(const Json& m, ModelCaps& c) {
    c.configName = jstr(m, { "config_name", "name", "configName" }, c.configName);
    c.modelName = jstr(m, { "model_name", "modelName", "raw_model_name" }, c.modelName.empty() ? c.configName : c.modelName);
    c.displayName = jstr(m, { "display_model_name", "display_name", "displayName" },
                          c.displayName.empty() ? c.configName : c.displayName);
    c.configSource = (int)jint(m, { "config_source", "configSource" }, c.configSource);
    c.provider = jstr(m, { "provider" }, c.provider);
    c.isPreset = m.get("is_preset", Json(true)).asBool(true);
    const Json* mm = m.find("max_mode");
    if (mm) c.maxMode = mm->asBool();
    mm = m.find("is_dollar_max");
    if (mm && mm->isBool()) c.maxMode = c.maxMode || mm->asBool();
    // display_config 内的 max_mode / display_name
    const Json* dc = m.find("display_config");
    if (dc && dc->isObject()) {
        const Json* dmm = dc->find("max_mode");
        if (dmm && dmm->isBool() && dmm->asBool()) c.maxMode = true;
        const Json* dn = dc->find("display_name");
        if (dn && dn->isString() && (c.displayName.empty() || c.displayName == c.configName))
            c.displayName = dn->asString();
    }
    // 计费倍率：display_contact_config 是 JSON 字符串，完整结构（2026-09-19 实测）：
    //   consumption_rate.data.rate = 基础倍率
    //   discount{subKey:"member_discount", data:{original_consumption_rate,
    //     consumption_rate(会员全天价), member_discount(折扣百分比), is_discount_matched}}
    //   activity_discount{subKey, data:{current{discount_type, before_consumption_rate,
    //     consumption_rate(活动价), discount}, member{after_consumption_rate(活动会员全天价)},
    //     off_peak{..., time_windows:[{weekdays[1..7], start_minute, end_minute}]（北京时间）}}}
    // 显示/计费按会员身份 + 时间窗门控（见 modelRateView），这里只做忠实解析。
    const Json* dcc = m.find("display_contact_config");
    if (dcc && dcc->isString() && !dcc->asString().empty()) {
        Json dccObj;
        if (Json::parse(dcc->asString(), dccObj) && dccObj.isObject()) {
            auto num = [](const Json* v) -> double {
                return v && v->isNumber() && v->asDouble() > 0 ? v->asDouble() : 0;
            };
            const Json* crObj = dccObj.find("consumption_rate");
            double rb = 0;
            if (crObj && crObj->isObject()) {
                const Json* cd = crObj->find("data");
                if (!cd || !cd->isObject()) cd = crObj;
                rb = num(cd->find("rate"));
            }
            if (rb > 0) c.rateBase = rb;
            const Json* dis = dccObj.find("discount");
            if (dis && dis->isObject()) {
                const Json* dd = dis->find("data");
                if (dd && dd->isObject()) {
                    double rm = num(dd->find("consumption_rate"));
                    if (rm > 0) {
                        c.rateMember = rm;
                        c.memberDiscountOff = (int)dd->get("member_discount", Json((double)0)).asDouble(0);
                    }
                }
            }
            const Json* ad = dccObj.find("activity_discount");
            if (ad && ad->isObject()) {
                const Json* add = ad->find("data");
                if (add && add->isObject()) {
                    const Json* cur = add->find("current");
                    if (cur && cur->isObject()) {
                        double r = num(cur->find("consumption_rate"));
                        if (r > 0) {
                            c.rateActivity = r;
                            std::string dt = cur->get("discount_type", Json("")).asString();
                            if (!dt.empty()) c.activityType = dt;
                        }
                        // 活动模型目录 rate 字段是折后现价，真原价在 before（2026-09-19 实测：
                        // evolving 限时 0.8→0.08 / turbo 补贴 0.4→0.2 / deepseek 闲时 0.16→0.08）
                        double b = num(cur->find("before_consumption_rate"));
                        if (b > 0) c.rateActivityBefore = b;
                    }
                    const Json* mem = add->find("member");
                    if (mem && mem->isObject()) {
                        double r = num(mem->find("after_consumption_rate"));
                        if (r > 0) c.rateActivityMember = r;
                    }
                    const Json* op = add->find("off_peak");
                    if (op && op->isObject()) {
                        const Json* tw = op->find("time_windows");
                        if (tw && tw->isArray()) {
                            for (size_t i = 0; i < tw->size(); ++i) {
                                const Json& w = tw->at(i);
                                if (!w.isObject()) continue;
                                ModelCaps::OffPeakWindow win;
                                win.startMinute = (int)w.get("start_minute", Json((double)0)).asDouble(0);
                                win.endMinute = (int)w.get("end_minute", Json((double)0)).asDouble(0);
                                const Json* wd = w.find("weekdays");
                                if (wd && wd->isArray())
                                    for (size_t k = 0; k < wd->size(); ++k)
                                        win.weekdays.push_back((int)wd->at(k).asDouble(0));
                                if (win.endMinute > win.startMinute) c.offPeakWindows.push_back(win);
                            }
                        }
                    }
                }
            }
        }
    }
    const Json* cws = m.find("context_window_size");
    if (cws && cws->isObject()) {
        c.cwDefault = jint(*cws, { "default" }, c.cwDefault);
        const Json* mx = cws->find("max");
        if (mx && mx->isArray()) {
            c.cwMax.clear();
            for (size_t i = 0; i < mx->size(); ++i) c.cwMax.push_back(mx->at(i).asInt(0));
        } else if (mx && mx->isNumber()) {
            c.cwMax = { mx->asInt() };
        }
    }
    const Json* tokens = m.find("context_window_tokens");
    if (tokens && tokens->isObject()) {
        long long dev = jint(*tokens, { "dev" }, 0);
        if (dev > 0) c.cwDefault = dev;
        long long mx = jint(*tokens, { "max" }, 0);
        if (mx > 0 && c.cwMax.empty()) c.cwMax = { mx };
    }
    c.promptMaxTokens = jint(m, { "prompt_max_tokens" }, c.promptMaxTokens);
    c.maxTokens = jint(m, { "max_tokens" }, c.maxTokens);
    c.maxTurn = (int)jint(m, { "max_turn" }, c.maxTurn);
    // features 可能是 JSON 字符串
    const Json* feat = m.find("features");
    Json featObj;
    if (feat && feat->isString()) Json::parse(feat->asString(), featObj);
    else if (feat && feat->isObject()) featObj = *feat;
    if (featObj.isObject()) {
        const Json* fcw = featObj.find("context_windows");
        if (fcw && fcw->isObject()) {
            const Json* fd = fcw->find("data");
            if (fd && fd->isObject()) {
                c.cwDefault = jint(*fd, { "dev_context" }, c.cwDefault);
                if (c.cwMax.empty()) {
                    long long mc = jint(*fd, { "max_context" }, 0);
                    if (mc) c.cwMax = { mc };
                }
                const Json* mcl = fd->find("max_context_list");
                if (mcl && mcl->isArray() && mcl->size() > 0) {
                    c.cwMax.clear();
                    for (size_t i = 0; i < mcl->size(); ++i) c.cwMax.push_back(mcl->at(i).asInt(0));
                }
                c.maxTurn = (int)jint(*fd, { "max_turns" }, c.maxTurn);
            }
        }
    }
    // R5：内置 reasoning_effort_config / 外部 reasoning_effort_options
    const Json* rec = m.find("reasoning_effort_config");
    if (rec && rec->isObject()) {
        bool support = rec->get("support_thinking", Json(false)).asBool(false);
        const Json* opts = rec->find("options");
        std::vector<std::string> parsed;
        if (support && opts && opts->isArray() && opts->size()) {
            bool bad = false;
            for (size_t i = 0; i < opts->size(); ++i) {
                if (!opts->at(i).isString()) { bad = true; break; }
                std::string v = normalizeEffort(opts->at(i).asString());
                if (v.empty()) { bad = true; break; }
                bool dup = false;
                for (auto& e : parsed)
                    if (e == v) dup = true;
                if (dup) { bad = true; break; }
                parsed.push_back(v);
            }
            if (!bad) {
                std::string defv;
                const Json* dl = rec->find("default_level");
                if (dl && dl->isString()) {
                    size_t a = dl->asString().find_first_not_of(" \t");
                    if (a != std::string::npos) defv = dl->asString().substr(a);
                    size_t b = defv.find_last_not_of(" \t");
                    if (b != std::string::npos) defv = defv.substr(0, b + 1);
                    else defv = "";
                }
                defv = normalizeEffort(defv);
                bool found = false;
                for (auto& e : parsed)
                    if (e == defv) found = true;
                if (!parsed.empty()) {
                    c.supportThinking = true;
                    c.effortOptions = parsed;
                    c.effortDefault = found ? defv : parsed.front();
                }
            }
        }
    }
    const Json* reo = m.find("reasoning_effort_options");
    if (reo && reo->isArray()) {
        static const char* wl[] = { "none", "light", "low", "medium", "high", "xhigh", "max" };
        for (size_t i = 0; i < reo->size(); ++i) {
            if (!reo->at(i).isString()) continue;
            std::string lv = normalizeEffort(reo->at(i).asString());
            for (auto w : wl) {
                if (lv == w && std::find(c.effortOptionsExt.begin(), c.effortOptionsExt.end(), lv) == c.effortOptionsExt.end())
                    c.effortOptionsExt.push_back(lv);
            }
        }
        if (!c.effortOptionsExt.empty()) {
            std::string de = jstr(m, { "default_reasoning_effort" }, "");
            if (!de.empty()) c.effortDefault = de;
            c.supportThinking = true;
        }
    }
    // 视觉能力：模型条目与 features 对象两处都扫；无明确信号默认 true 透传。
    bool sawSignal = false, vision = false;
    detectVision(m, sawSignal, vision);
    if (featObj.isObject()) detectVision(featObj, sawSignal, vision);
    c.vision = sawSignal ? vision : true;
    c.present = true;
}

// 同一模型会按 function 分组返回多份档案。保留一条公开模型记录，
// 但把各档案的能力做并集，尤其不能丢掉只在某一档案中出现的 Max 标记。
static void mergeCaps(ModelCaps& dst, const ModelCaps& src) {
    if (dst.modelName.empty()) dst.modelName = src.modelName;
    if (dst.displayName.empty()) dst.displayName = src.displayName;
    if (dst.provider.empty()) dst.provider = src.provider;
    if (dst.configSource == 0) dst.configSource = src.configSource;
    dst.maxMode = dst.maxMode || src.maxMode;
    dst.ideChatCapable = dst.ideChatCapable || src.ideChatCapable;
    if (dst.ideFunction.empty() && !src.ideFunction.empty()) dst.ideFunction = src.ideFunction;
    if (src.ideChatCapable &&
        src.modelName.find("__dev") != std::string::npos)
        dst.modelName = src.modelName;
    if (dst.maxModelName.empty() && !src.maxModelName.empty()) dst.maxModelName = src.maxModelName;
    dst.present = dst.present || src.present;
    if (src.cwDefault > dst.cwDefault) dst.cwDefault = src.cwDefault;
    for (auto v : src.cwMax) {
        if (std::find(dst.cwMax.begin(), dst.cwMax.end(), v) == dst.cwMax.end()) dst.cwMax.push_back(v);
    }
    if (src.promptMaxTokens > dst.promptMaxTokens) dst.promptMaxTokens = src.promptMaxTokens;
    if (src.maxTokens > dst.maxTokens) dst.maxTokens = src.maxTokens;
    if (src.maxTurn > dst.maxTurn) dst.maxTurn = src.maxTurn;
    dst.supportThinking = dst.supportThinking || src.supportThinking;
    // 视觉能力取并集；仅当两份档案都给出过明确 false 信号时才关闭（默认 true）
    dst.vision = dst.vision || src.vision;
    auto mergeStrings = [](std::vector<std::string>& out, const std::vector<std::string>& in) {
        for (const auto& v : in)
            if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    };
    mergeStrings(dst.effortOptions, src.effortOptions);
    mergeStrings(dst.effortOptionsExt, src.effortOptionsExt);
    if (dst.effortDefault.empty() && !src.effortDefault.empty()) dst.effortDefault = src.effortDefault;
    if (dst.rateBase == 0.0 && src.rateBase > 0) dst.rateBase = src.rateBase;
    if (dst.rateMember == 0.0 && src.rateMember > 0) dst.rateMember = src.rateMember;
    if (dst.memberDiscountOff == 0 && src.memberDiscountOff > 0) dst.memberDiscountOff = src.memberDiscountOff;
    if (dst.rateActivity == 0.0 && src.rateActivity > 0) dst.rateActivity = src.rateActivity;
    if (dst.rateActivityBefore == 0.0 && src.rateActivityBefore > 0) dst.rateActivityBefore = src.rateActivityBefore;
    if (dst.rateActivityMember == 0.0 && src.rateActivityMember > 0) dst.rateActivityMember = src.rateActivityMember;
    if (dst.activityType.empty() && !src.activityType.empty()) dst.activityType = src.activityType;
    if (dst.offPeakWindows.empty() && !src.offPeakWindows.empty()) dst.offPeakWindows = src.offPeakWindows;
}

std::vector<ModelCaps> ModelCatalog::all(Account&, std::string& err) {
    std::unique_lock<std::mutex> lk(m_mtx);
    m_wake.wait(lk, [this] { return m_attempted; });
    err = m_cacheErr;
    return m_cache;
}

bool ModelCatalog::refreshOnce(Account& acc, std::string& err) {
    auto cfg = settings::get();
    std::vector<ModelCaps> merged;

    // get_detail_param 是 Trae 当前 chat_v3 可见模型的权威目录，
    // 同时携带 reasoning_effort_config / max_mode / __max 档案信息。
    std::vector<std::string> primaryVisibleNames;
    bool primaryCatalogOk = false;
    bool allCatalogsOk = true;
    http::Headers hd = ideHeaders(acc, cfg->ideVersion, cfg->ideVersionCode, false);
    auto loadDetailCatalog = [&](const char* functionName) {
        Json body = Json::object();
        body.set("function", Json(functionName));
        body.set("config_names", Json(nullptr));
        body.set("need_prompt", Json(false));
        body.set("current_config_info", Json(nullptr));
        body.set("poly_prompt", Json(true));
        body.set("mode_type", Json(nullptr));
        body.set("agent_type", Json(nullptr));
        auto resp = http::send("POST", "https://trae-api-cn.mchost.guru/api/ide/v1/get_detail_param",
                                hd, body.dump(), 30000);
        if (!resp.ok()) {
            allCatalogsOk = false;
            err = std::string(functionName) + " HTTP " + std::to_string(resp.status) + ": " + resp.error;
            LOG_W("get_detail_param(%s) 拉取失败: HTTP %d err=%s body=%s", functionName,
                  resp.status, resp.error.c_str(), resp.body.substr(0, 200).c_str());
            return;
        }
        Json j;
        if (!Json::parse(resp.body, j)) {
            allCatalogsOk = false;
            err = std::string(functionName) + " 响应不是合法 JSON";
            LOG_W("get_detail_param(%s) 响应不是合法 JSON", functionName);
            return;
        }
        const Json* data = j.find("data");
        const Json& root = data && data->isObject() ? *data : j;
        const Json* cil = root.find("config_info_list");
        if (!cil || !cil->isArray()) {
            allCatalogsOk = false;
            err = std::string(functionName) + " 响应缺少模型列表";
            return;
        }
        const bool primaryCatalog = _stricmp(functionName, "chat_v3") == 0 ||
                                    _stricmp(functionName, "solo_agent") == 0;
        if (primaryCatalog) primaryCatalogOk = true;
        for (size_t ci = 0; ci < cil->size(); ++ci) {
            const Json& cfgItem = cil->at(ci);
            std::string cfgName = jstr(cfgItem, { "config_name" });
            // solo_coder 是 Trae Code 模式自己的模型表（当前不加载）；
            // chat_v3/solo_agent 按 config_switch 与 invisible 标记收敛。
            bool hidden = jsonBool(cfgItem, "is_invisible_to_user") || !jsonBool(cfgItem, "config_switch", true);
            bool custom = false;
            bool parentMax = jsonBool(cfgItem, "max_mode");
            const Json* parentDisplay = cfgItem.find("display_config");
            if (parentDisplay && parentDisplay->isObject()) {
                custom = jsonBool(*parentDisplay, "is_custom_model");
                parentMax = parentMax || jsonBool(*parentDisplay, "max_mode");
            }
            if (hidden || custom || cfgName.empty()) continue;
            long long detailSource = jint(cfgItem, { "config_source", "configSource" }, 1);
            if (primaryCatalog && detailSource == 1 && !isInternalModelName(cfgName)) {
                bool duplicate = false;
                for (const auto& v : primaryVisibleNames) {
                    if (_stricmp(v.c_str(), cfgName.c_str()) == 0) { duplicate = true; break; }
                }
                if (!duplicate) primaryVisibleNames.push_back(cfgName);
            }
            const Json* mdl = cfgItem.find("model_detail_list");
            if (!mdl || !mdl->isArray()) continue;
            for (size_t mi = 0; mi < mdl->size(); ++mi) {
                const Json& md = mdl->at(mi);
                ModelCaps c;
                // 公共能力在 config_info 条目，档位参数在 model_detail_list，两层合并。
                c.configName = cfgName;
                capsFromModelJson(cfgItem, c);
                c.configName = cfgName;
                capsFromModelJson(md, c);
                if (c.configName.empty()) continue;
                // 详情目录来自 IDE 通道，走 chat_v3/solo_agent function。
                c.ideChatCapable = true;
                std::string rawChatFunction = jstr(md, { "raw_chat_function", "rawChatFunction" });
                c.ideFunction = rawChatFunction.empty() ? functionName : rawChatFunction;
                if (isInternalModelName(c.configName) || c.configSource != 1) continue;
                if (parentMax) c.maxMode = true;
                std::string detailModelName = jstr(md, { "model_name", "modelName" });
                bool isMaxDetail = md.get("max_mode", Json(false)).asBool(false);
                if (!isMaxDetail && detailModelName.size() >= 5 &&
                    detailModelName.compare(detailModelName.size() - 5, 5, "__max") == 0)
                    isMaxDetail = true;
                if (isMaxDetail && !detailModelName.empty()) c.maxModelName = detailModelName;
                if (!md.find("config_source") && cfgItem.find("config_source"))
                    c.configSource = (int)cfgItem.find("config_source")->asInt(1);
                if (c.configSource != 1) continue;
                ModelCaps* tgt = nullptr;
                for (auto& e : merged) {
                    std::string a = e.configName, b = c.configName;
                    for (auto& ch : a) ch = (char)tolower((unsigned char)ch);
                    for (auto& ch : b) ch = (char)tolower((unsigned char)ch);
                    if (a == b) { tgt = &e; break; }
                }
                if (!tgt) merged.push_back(c);
                else mergeCaps(*tgt, c);
            }
        }
    };
    // Trae 当前模型选择器把 chat_v3 / solo_agent 映射成用户可选的主模型目录。
    loadDetailCatalog("chat_v3");
    loadDetailCatalog("solo_agent");
    if (!allCatalogsOk) return false;

    // 按主目录收敛，避免把其他功能目录的专用/已下线模型暴露给客户端。
    if (primaryCatalogOk && !primaryVisibleNames.empty()) {
        auto visible = [&](const std::string& name) {
            for (const auto& v : primaryVisibleNames)
                if (_stricmp(v.c_str(), name.c_str()) == 0) return true;
            return false;
        };
        merged.erase(std::remove_if(merged.begin(), merged.end(),
                                    [&](const ModelCaps& c) { return !visible(c.configName); }),
                     merged.end());
    }
    if (merged.empty()) {
        err = "模型表为空（账号未登录或上游不可达）";
        std::lock_guard<std::mutex> lk(m_mtx);
        m_cacheErr = err;
        return false;
    }
    int modelCount = (int)merged.size();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_cache = std::move(merged);
        m_cacheErr.clear();
    }
    m_everOk.store(true);
    m_version.fetch_add(1);
    LOG_I("模型表已刷新：%d 个模型", modelCount);
    return true;
}

std::vector<ModelCaps> ModelCatalog::cached() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_cache;
}

void ModelCatalog::start() {
    if (m_worker.joinable()) return;
    m_worker = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::string err;
            bool ok = false;
            auto& pool = AccountPool::instance();
            // 元数据请求不占聊天并发槽，避免首个聊天等待目录时互相阻塞。
            if (pool.accounts().empty()) {
                err = "未发现可用账号";
            } else {
                auto acc = pool.accounts().front();
                pool.ensureFreshToken(*acc);
                ok = refreshOnce(*acc, err);
            }
            if (!ok) LOG_W("模型表刷新失败：%s；5 分钟后重试", err.c_str());
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cacheErr = err;
            m_attempted = true;
            m_wake.notify_all();
            m_wake.wait_for(lk, stop, std::chrono::seconds(ok ? 3600 : 300), [] { return false; });
        }
    });
}

void ModelCatalog::stop() {
    m_worker.request_stop();
    if (m_worker.joinable()) m_worker.join();
}

bool ModelCatalog::get(Account& acc, const std::string& configName, ModelCaps& out, std::string& err) {
    auto all = this->all(acc, err);
    for (auto& c : all) {
        std::string a = c.configName, b = configName;
        for (auto& ch : a) ch = (char)tolower((unsigned char)ch);
        for (auto& ch : b) ch = (char)tolower((unsigned char)ch);
        if (a == b) {
            out = c;
            return true;
        }
    }
    // display_name 模糊匹配
    for (auto& c : all) {
        std::string d = c.displayName;
        std::string b = configName;
        for (auto& ch : d) ch = (char)tolower((unsigned char)ch);
        for (auto& ch : b) ch = (char)tolower((unsigned char)ch);
        if (!b.empty() && d.find(b) != std::string::npos) {
            out = c;
            return true;
        }
    }
    return false;
}

ModelCatalog& ModelCatalog::instance() {
    static ModelCatalog c;
    return c;
}
