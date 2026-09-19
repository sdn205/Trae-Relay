// Config.cpp - 配置装载/落盘/热加载（config.json 固定位于 exe 同目录）
#include "app/Config.h"
#include "app/ConfigSchema.h"
#include "common/Crypto.h"
#include "common/Log.h"
#include "accounts/Storage.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>

// 探测不到本机 Trae 时的最终兜底（随版本发布更新；正常路径下版本头来自
// 本机 Trae 安装目录的 product.json，不依赖这两个常量）。
static const char* kFallbackIdeVersion = "3.3.102";
static const char* kFallbackIdeVersionCode = "20260916";

static std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string appDataDir() {
    wchar_t path[MAX_PATH] = { 0 };
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path) != S_OK) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, &out[0], n, nullptr, nullptr);
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::string Config::resolvePath(const std::string& explicitPath) {
    if (!explicitPath.empty()) return explicitPath;
    return exeDir() + "\\config.json";
}

Config& Config::instance() {
    static Config c;
    return c;
}

void Config::applyJson(const Json& j) {
    version = (int)j.get("version", Json(1)).asInt(1);
    const Json* s = j.find("service");
    if (s && s->isObject()) {
        serviceHost = s->get("host", Json(serviceHost)).asString();
        servicePort = (int)s->get("port", Json(servicePort)).asInt(servicePort);
        allowLan = s->get("allowLan", Json(allowLan)).asBool(allowLan);
        apiKey = s->get("apiKey", Json(apiKey)).asString();
        allowAnyApiKey = s->get("allowAnyApiKey", Json(false)).asBool(false);
        apiKeyProtected = s->get("apiKeyProtected", Json(apiKeyProtected)).asBool(apiKeyProtected);
        upstreamTimeoutSec = (int)s->get("upstreamTimeoutSec", Json(upstreamTimeoutSec)).asInt(upstreamTimeoutSec);
        firstEventTimeoutSec = (int)s->get("firstEventTimeoutSec", Json(firstEventTimeoutSec)).asInt(firstEventTimeoutSec);
        maxConcurrentPerAccount = (int)s->get("maxConcurrentPerAccount", Json(maxConcurrentPerAccount)).asInt(maxConcurrentPerAccount);
        minRequestIntervalMs = (int)s->get("minRequestIntervalMs", Json(minRequestIntervalMs)).asInt(minRequestIntervalMs);
        if (upstreamTimeoutSec < 10) upstreamTimeoutSec = 10;
        if (upstreamTimeoutSec > 1800) upstreamTimeoutSec = 1800;
        if (firstEventTimeoutSec < 10) firstEventTimeoutSec = 10;
        if (firstEventTimeoutSec > 1800) firstEventTimeoutSec = 1800;
        if (maxConcurrentPerAccount < 1) maxConcurrentPerAccount = 1;
        if (maxConcurrentPerAccount > 8) maxConcurrentPerAccount = 8;
        if (minRequestIntervalMs < 0) minRequestIntervalMs = 0;
        if (servicePort <= 0 || servicePort > 65535) servicePort = cfg::kDefaultPort;
    }
    const Json* st = j.find("startup");
    if (st && st->isObject()) {
        autoStart = st->get("autoStart", Json(autoStart)).asBool(autoStart);
        startMinimizedToTray = st->get("startMinimizedToTray", Json(startMinimizedToTray)).asBool(startMinimizedToTray);
        minimizeToTrayOnClose = st->get("minimizeToTrayOnClose", Json(minimizeToTrayOnClose)).asBool(minimizeToTrayOnClose);
        trayClickAction = st->get("trayClickAction", Json(trayClickAction)).asString();
        singleInstance = st->get("singleInstance", Json(singleInstance)).asBool(singleInstance);
        if (trayClickAction != "show" && trayClickAction != "none") trayClickAction = "show";
    }
    // upstream：固定 solo 通道，仅保留 IDE 版本头，使用当前 IDE 默认值；
    // 不从旧版 channel.ideVersion.solo 迁移（那是更早的 0.1.x 头）。
    const Json* up = j.find("upstream");
    if (up && up->isObject()) {
        ideVersion = up->get("ideVersion", Json(ideVersion)).asString();
        ideVersionCode = up->get("ideVersionCode", Json(ideVersionCode)).asString();
    }
    const Json* d = j.find("defaults");
    if (d && d->isObject()) {
        const Json* re = d->find("reasoningEffort");
        if (re && re->isString() && cfg::isValidEffort(re->asString())) defaultReasoningEffort = re->asString();
        else if (re && re->isNull()) defaultReasoningEffort = "";
        defaultIsMaxMode = (int)d->get("isMaxMode", Json(defaultIsMaxMode)).asInt(defaultIsMaxMode) ? 1 : 0;
        defaultMaxContextWindow = d->get("maxContextWindow", Json((double)defaultMaxContextWindow)).asInt(defaultMaxContextWindow);
        defaultStream = d->get("stream", Json(defaultStream)).asBool(defaultStream);
        autoContinue = d->get("autoContinue", Json(autoContinue)).asBool(autoContinue);
        maxAutoContinue = (int)d->get("maxAutoContinue", Json(maxAutoContinue)).asInt(maxAutoContinue);
    }
    const Json* rs = j.find("responses");
    if (rs && rs->isObject()) {
        responsesEnabled = rs->get("enabled", Json(responsesEnabled)).asBool(responsesEnabled);
        responsesMapReasoningSummary = rs->get("mapReasoningSummary", Json(responsesMapReasoningSummary)).asBool(responsesMapReasoningSummary);
        responsesSessionCacheSize = (int)rs->get("sessionCacheSize", Json(responsesSessionCacheSize)).asInt(responsesSessionCacheSize);
        responsesSessionCachePersist = rs->get("sessionCachePersist", Json(responsesSessionCachePersist)).asBool(responsesSessionCachePersist);
    }
    const Json* ac = j.find("accounts");
    if (ac && ac->isObject()) {
        accountsAutoDiscover = ac->get("autoDiscover", Json(accountsAutoDiscover)).asBool(accountsAutoDiscover);
        const Json* ci = ac->find("checkin");
        if (ci && ci->isObject()) {
            checkinEnabled = ci->get("enabled", Json(checkinEnabled)).asBool(checkinEnabled);
            checkinHour = (int)ci->get("hour", Json(checkinHour)).asInt(checkinHour);
            checkinMinute = (int)ci->get("minute", Json(checkinMinute)).asInt(checkinMinute);
            if (checkinHour < 0) checkinHour = 0;
            if (checkinHour > 23) checkinHour = 23;
            if (checkinMinute < 0) checkinMinute = 0;
            if (checkinMinute > 59) checkinMinute = 59;
        }
        const Json* po = ac->find("pool");
        if (po && po->isObject()) {
            poolSelectBy = po->get("selectBy", Json(poolSelectBy)).asString();
            poolCooldownSec = (int)po->get("cooldownSec", Json(poolCooldownSec)).asInt(poolCooldownSec);
            poolDisableAfterFails = (int)po->get("disableAfterFails", Json(poolDisableAfterFails)).asInt(poolDisableAfterFails);
            if (poolSelectBy != "credits" && poolSelectBy != "roundRobin") poolSelectBy = "credits";
            if (poolCooldownSec < 5) poolCooldownSec = 5;
            if (poolDisableAfterFails < 1) poolDisableAfterFails = 1;
        }
    }
    const Json* lg = j.find("logging");
    if (lg && lg->isObject()) {
        logLevel = lg->get("level", Json(logLevel)).asString();
        logDir = lg->get("dir", Json(logDir)).asString();
        logRetainDays = (int)lg->get("retainDays", Json(logRetainDays)).asInt(logRetainDays);
        logRedactSecrets = lg->get("redactSecrets", Json(logRedactSecrets)).asBool(logRedactSecrets);
        if (logLevel != "trace" && logLevel != "debug" && logLevel != "info" && logLevel != "warn" && logLevel != "error") logLevel = "info";
    }
    const Json* ms = j.find("models");
    if (ms && ms->isObject()) {
        for (auto& kv : ms->members()) {
            const Json& m = kv.second;
            ModelConfig mc;
            mc.present = true;
            mc.name = kv.first;
            mc.enabled = m.get("enabled", Json(true)).asBool(true);
            mc.displayName = m.get("displayName", Json("")).asString();
            const Json* al = m.find("alias");
            if (al && al->isArray())
                for (size_t i = 0; i < al->size(); ++i)
                    if (al->at(i).isString()) mc.alias.push_back(al->at(i).asString());
            const Json* re = m.find("reasoningEffort");
            if (re && re->isString() && cfg::isValidEffort(re->asString())) mc.reasoningEffort = re->asString();
            mc.isMaxMode = m.get("isMaxMode", Json(0)).asInt(0) ? 1 : 0;
            mc.maxContextWindow = m.get("maxContextWindow", Json((double)0)).asInt(0);
            mc.extraSystemPrefix = m.get("extraSystemPrefix", Json("")).asString();
            upsertModel(mc);
        }
    }
}

bool Config::load(const std::string& explicitPath, std::string& err) {
    path = resolvePath(explicitPath);
    // 首次以 exe 同目录启动：若旧版本配置在 %APPDATA%\TraeRelay，原样迁移，
    // 保留 apiKey 与模型档位设置（旧字段在保存时自然丢弃）。
    DWORD attr = GetFileAttributesW(widenUtf8(path).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::string appData = appDataDir();
        if (!appData.empty()) {
            std::string oldPath = appData + "\\TraeRelay\\config.json";
            if (GetFileAttributesW(widenUtf8(oldPath).c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (CopyFileW(widenUtf8(oldPath).c_str(), widenUtf8(path).c_str(), FALSE))
                    LOG_I("已迁移旧配置: %s -> %s", oldPath.c_str(), path.c_str());
            }
        }
    }
    HANDLE h = CreateFileW(widenUtf8(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // 首次启动：生成默认配置
        if (apiKey.empty()) apiKey = crypto::genApiKey();
        resolveIdeVersion();
        return save(err);
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string text;
    if (sz.QuadPart > 0 && sz.QuadPart < 16 * 1024 * 1024) {
        text.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        ReadFile(h, &text[0], (DWORD)sz.QuadPart, &rd, nullptr);
    }
    CloseHandle(h);
    Json j;
    if (!Json::parse(text, j, &err)) {
        err = "配置文件解析失败: " + err;
        return false;
    }
    if (version > 1 && j.get("version", Json(1)).asInt(1) > version) readOnlyMode = true;
    rawUnknown = j; // 保留原始树
    applyJson(j);
    if (apiKey.empty()) apiKey = crypto::genApiKey();
    resolveIdeVersion();
    return true;
}

void Config::resolveIdeVersion() {
    // 版本头不写死：优先配置显式覆盖，其次从本机 Trae 安装的 product.json
    // 动态读取（appVersion + 构建日期），最后才用编译期兜底。这样 Trae 更新
    // 后无需同步修改本程序也不会被版本头卡住。
    if (!ideVersion.empty() && !ideVersionCode.empty()) return;
    std::string ver, code;
    if (Storage::detectIdeVersion(ver, code)) {
        if (ideVersion.empty()) ideVersion = ver;
        if (ideVersionCode.empty() && !code.empty()) ideVersionCode = code;
        LOG_I("已从本机 Trae 读取版本头: %s / %s", ideVersion.c_str(), ideVersionCode.c_str());
    }
    if (ideVersion.empty()) ideVersion = kFallbackIdeVersion;
    if (ideVersionCode.empty()) ideVersionCode = kFallbackIdeVersionCode;
}

Json Config::toJson() const {
    Json j = rawUnknown.isObject() ? rawUnknown : Json::object();
    // 已移除的旧段不再写回
    j.erase("channel");
    j.erase("search");
    j.set("version", Json(version));
    Json svc = j.find("service") && j.find("service")->isObject() ? *j.find("service") : Json::object();
    svc.set("host", Json(serviceHost));
    svc.set("port", Json(servicePort));
    svc.set("allowLan", Json(allowLan));
    svc.set("apiKey", Json(apiKey));
    svc.set("allowAnyApiKey", Json(allowAnyApiKey));
    svc.set("apiKeyProtected", Json(apiKeyProtected));
    svc.set("upstreamTimeoutSec", Json(upstreamTimeoutSec));
    svc.set("firstEventTimeoutSec", Json(firstEventTimeoutSec));
    svc.set("maxConcurrentPerAccount", Json(maxConcurrentPerAccount));
    svc.set("minRequestIntervalMs", Json(minRequestIntervalMs));
    j.set("service", svc);

    Json su = j.find("startup") && j.find("startup")->isObject() ? *j.find("startup") : Json::object();
    su.set("autoStart", Json(autoStart));
    su.set("startMinimizedToTray", Json(startMinimizedToTray));
    su.set("minimizeToTrayOnClose", Json(minimizeToTrayOnClose));
    su.set("trayClickAction", Json(trayClickAction));
    su.set("singleInstance", Json(singleInstance));
    j.set("startup", su);

    // 版本头每次启动从本机 Trae 动态探测，不写回配置，避免 Trae 更新后
    // 被配置里固化的旧版本头卡住；旧配置里的 upstream 段直接丢弃。
    j.erase("upstream");

    Json d = j.find("defaults") && j.find("defaults")->isObject() ? *j.find("defaults") : Json::object();
    d.erase("effortMode");
    if (defaultReasoningEffort.empty()) d.set("reasoningEffort", Json(nullptr));
    else d.set("reasoningEffort", Json(defaultReasoningEffort));
    d.set("isMaxMode", Json(defaultIsMaxMode));
    if (defaultMaxContextWindow) d.set("maxContextWindow", Json(defaultMaxContextWindow));
    else if (d.find("maxContextWindow")) d.set("maxContextWindow", Json(nullptr));
    d.set("stream", Json(defaultStream));
    d.set("autoContinue", Json(autoContinue));
    d.set("maxAutoContinue", Json(maxAutoContinue));
    j.set("defaults", d);

    Json rs = j.find("responses") && j.find("responses")->isObject() ? *j.find("responses") : Json::object();
    rs.set("enabled", Json(responsesEnabled));
    rs.set("mapReasoningSummary", Json(responsesMapReasoningSummary));
    rs.set("sessionCacheSize", Json(responsesSessionCacheSize));
    rs.set("sessionCachePersist", Json(responsesSessionCachePersist));
    j.set("responses", rs);

    Json ac = j.find("accounts") && j.find("accounts")->isObject() ? *j.find("accounts") : Json::object();
    ac.set("autoDiscover", Json(accountsAutoDiscover));
    Json ci = ac.find("checkin") && ac.find("checkin")->isObject() ? *ac.find("checkin") : Json::object();
    ci.set("enabled", Json(checkinEnabled));
    ci.set("hour", Json(checkinHour));
    ci.set("minute", Json(checkinMinute));
    ac.set("checkin", ci);
    Json po = ac.find("pool") && ac.find("pool")->isObject() ? *ac.find("pool") : Json::object();
    po.set("selectBy", Json(poolSelectBy));
    po.set("cooldownSec", Json(poolCooldownSec));
    po.set("disableAfterFails", Json(poolDisableAfterFails));
    ac.set("pool", po);
    j.set("accounts", ac);

    Json lg = j.find("logging") && j.find("logging")->isObject() ? *j.find("logging") : Json::object();
    lg.set("level", Json(logLevel));
    lg.set("dir", Json(logDir));
    lg.set("retainDays", Json(logRetainDays));
    lg.set("redactSecrets", Json(logRedactSecrets));
    j.set("logging", lg);

    Json ms = Json::object();
    for (auto& m : models) {
        Json mj;
        mj.set("enabled", Json(m.enabled));
        if (!m.displayName.empty()) mj.set("displayName", Json(m.displayName));
        if (!m.alias.empty()) {
            Json al = Json::array();
            for (auto& a : m.alias) al.push_back(Json(a));
            mj.set("alias", al);
        }
        if (!m.reasoningEffort.empty()) mj.set("reasoningEffort", Json(m.reasoningEffort));
        mj.set("isMaxMode", Json(m.isMaxMode));
        if (m.maxContextWindow) mj.set("maxContextWindow", Json(m.maxContextWindow));
        if (!m.extraSystemPrefix.empty()) mj.set("extraSystemPrefix", Json(m.extraSystemPrefix));
        ms.set(m.name, mj);
    }
    j.set("models", ms);
    return j;
}

bool Config::save(std::string& err) {
    if (readOnlyMode) {
        err = "配置版本高于程序支持，只读模式";
        return false;
    }
    std::string dir = path.substr(0, path.rfind('\\'));
    CreateDirectoryW(widenUtf8(dir).c_str(), nullptr);
    std::string tmp = path + ".tmp";
    std::string data = toJson().dump(true);
    HANDLE h = CreateFileW(widenUtf8(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "无法写配置临时文件: " + tmp;
        return false;
    }
    DWORD wr = 0;
    WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    if (!MoveFileExW(widenUtf8(tmp).c_str(), widenUtf8(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = "配置原子替换失败";
        DeleteFileW(widenUtf8(tmp).c_str());
        return false;
    }
    return true;
}

const ModelConfig* Config::modelConfig(const std::string& name) const {
    std::string ln = name;
    for (auto& c : ln) c = (char)tolower((unsigned char)c);
    for (auto& m : models) {
        std::string mn = m.name;
        for (auto& c : mn) c = (char)tolower((unsigned char)c);
        if (mn == ln) return &m;
        for (auto& a : m.alias) {
            std::string la = a;
            for (auto& c : la) c = (char)tolower((unsigned char)c);
            if (la == ln) return &m;
        }
    }
    return nullptr;
}

void Config::upsertModel(const ModelConfig& m) {
    for (auto& e : models)
        if (e.name == m.name) { e = m; return; }
    models.push_back(m);
}

// Config → core 设置快照：逐字段拷贝，core 从此只认这份不可变数据。
std::shared_ptr<const CoreSettings> makeCoreSettings(const Config& cfg) {
    auto s = std::make_shared<CoreSettings>();
    s->ideVersion = cfg.ideVersion;
    s->ideVersionCode = cfg.ideVersionCode;
    s->upstreamTimeoutSec = cfg.upstreamTimeoutSec;
    s->firstEventTimeoutSec = cfg.firstEventTimeoutSec;
    s->serviceHost = cfg.serviceHost;
    s->servicePort = cfg.servicePort;
    s->allowLan = cfg.allowLan;
    s->apiKey = cfg.apiKey;
    s->allowAnyApiKey = cfg.allowAnyApiKey;
    s->defaultReasoningEffort = cfg.defaultReasoningEffort;
    s->defaultIsMaxMode = cfg.defaultIsMaxMode;
    s->defaultMaxContextWindow = cfg.defaultMaxContextWindow;
    s->defaultStream = cfg.defaultStream;
    s->maxConcurrentPerAccount = cfg.maxConcurrentPerAccount;
    s->minRequestIntervalMs = cfg.minRequestIntervalMs;
    s->poolSelectBy = cfg.poolSelectBy;
    s->poolCooldownSec = cfg.poolCooldownSec;
    s->poolDisableAfterFails = cfg.poolDisableAfterFails;
    s->responsesEnabled = cfg.responsesEnabled;
    s->responsesMapReasoningSummary = cfg.responsesMapReasoningSummary;
    s->responsesSessionCacheSize = cfg.responsesSessionCacheSize;
    s->responsesSessionCachePersist = cfg.responsesSessionCachePersist;
    s->models = cfg.models;
    return s;
}
