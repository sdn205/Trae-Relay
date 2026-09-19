// Storage.cpp - storage.json 双格式兼容 + tc 解密（CNG）
#include "accounts/Storage.h"
#include "common/Crypto.h"
#include "common/Log.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <ctime>

#pragma comment(lib, "shell32.lib")

std::string exeDir() {
    wchar_t buf[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t s = p.rfind(L'\\');
    std::wstring dir = s == std::wstring::npos ? L"." : p.substr(0, s);
    int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), (int)dir.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), (int)dir.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// 四个 64 字节硬编码盐（来自 Trae CN 前端 JS 逆向）
static const uint8_t SALT_A[64] = {
    82, 9, 106, 213, 48, 54, 165, 56, 191, 64, 163, 158, 129, 243, 215, 251,
    124, 227, 57, 130, 155, 47, 255, 135, 52, 142, 67, 68, 196, 222, 233, 203,
    84, 123, 148, 50, 166, 194, 35, 61, 238, 76, 149, 11, 66, 250, 195, 78,
    8, 46, 161, 102, 40, 217, 36, 178, 118, 91, 162, 73, 109, 139, 209, 37,
};
static const uint8_t SALT_B[64] = {
    31, 221, 168, 51, 136, 7, 199, 49, 177, 18, 16, 89, 39, 128, 236, 95,
    96, 81, 127, 169, 25, 181, 74, 13, 45, 229, 122, 159, 147, 201, 156, 239,
    160, 224, 59, 77, 174, 42, 245, 176, 200, 235, 187, 60, 131, 83, 153, 97,
    23, 43, 4, 126, 186, 119, 214, 38, 225, 105, 20, 99, 85, 33, 12, 125,
};
static const uint8_t SALT_C[64] = {
    191, 192, 216, 250, 122, 246, 220, 97, 31, 254, 98, 27, 8, 72, 71, 176,
    135, 99, 96, 18, 127, 101, 203, 104, 211, 102, 191, 125, 37, 72, 150, 156,
    51, 229, 121, 35, 17, 153, 141, 177, 110, 131, 150, 128, 172, 255, 254, 6,
    18, 140, 55, 62, 236, 249, 135, 64, 135, 12, 117, 4, 89, 149, 168, 209,
};
static const uint8_t SALT_D[64] = {
    246, 204, 26, 232, 232, 70, 129, 109, 223, 146, 169, 242, 23, 241, 105, 145,
    50, 196, 165, 42, 254, 120, 3, 54, 244, 207, 209, 85, 53, 6, 138, 106,
    175, 148, 31, 204, 186, 186, 165, 182, 87, 142, 49, 10, 39, 110, 26, 154,
    86, 56, 173, 125, 18, 64, 198, 225, 99, 99, 83, 82, 191, 134, 76, 170,
};

// UTF-8 → UTF-16（%APPDATA% 在中文用户名路径下必须走 W 系列 API）
static std::wstring widenPath(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string appDataDir() {
    wchar_t path[MAX_PATH] = { 0 };
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path) != S_OK) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string out(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::vector<TraeEdition> Storage::discoverEditions() {
    std::string base = appDataDir();
    std::vector<TraeEdition> out;
    const char* dirs[][2] = {
        { "Trae CN", "cn" },
        { "TRAE SOLO CN", "solo" },
        { "Trae Work CN", "work" },
        { "Trae", "sg" },
        { "TRAE SOLO", "solo-sg" },
    };
    for (auto& d : dirs) {
        std::string userDir = base + "\\" + d[0] + "\\User";
        std::string f = userDir + "\\globalStorage\\storage.json";
        DWORD attr = GetFileAttributesW(widenPath(f).c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            out.push_back({ d[1], userDir, d[0] });
        }
    }
    return out;
}

std::string Storage::readMachineId(const std::string& userDir) {
    // 1) storage.json telemetry.machineId
    std::string sp = userDir + "\\globalStorage\\storage.json";
    HANDLE h = CreateFileW(widenPath(sp).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        if (sz.QuadPart > 0 && sz.QuadPart < 64 * 1024 * 1024) {
            std::string text((size_t)sz.QuadPart, 0);
            DWORD rd = 0;
            ReadFile(h, &text[0], (DWORD)sz.QuadPart, &rd, nullptr);
            CloseHandle(h);
            Json j;
            if (Json::parse(text, j)) {
                const Json* t = j.find("telemetry.machineId");
                if (t && t->isString() && t->asString().size() >= 32) return t->asString();
            }
        } else {
            CloseHandle(h);
        }
    } else {
        // 文件可能被 Trae 独占，退回共享读失败→继续尝试 machineid
    }
    // 2) machineid 文件
    std::string mp = userDir.substr(0, userDir.rfind("\\User")) + "\\machineid";
    h = CreateFileW(widenPath(mp).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[128] = { 0 };
        DWORD rd = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
        CloseHandle(h);
        std::string s(buf);
        // 去掉空白
        s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return isspace(c); }), s.end());
        if (!s.empty()) return s;
    }
    return "";
}

// AHA/TTNet 设备 ID：发行版根目录 aha\TinyStorage 里 "aha.device.device_id" 是
// tc 加密 blob，明文 JSON 形如 {"device_id_str":"458975...","install_id_str":...}。
// 上游发积分类接口（签到 claim）校验该 ID 是否为账号注册过的设备，不认识会被
// 9074「当前参与用户太多」软拒——它不是机器 machineid，也不是 devDeviceId。
std::string Storage::readAhaDeviceId(const std::string& userDir) {
    std::string root = userDir;
    size_t p = root.rfind("\\User");
    if (p != std::string::npos) root = root.substr(0, p);
    std::string fp = root + "\\aha\\TinyStorage";
    HANDLE h = CreateFileW(widenPath(fp).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string text;
    if (sz.QuadPart > 0 && sz.QuadPart < 1024 * 1024) {
        text.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        if (ReadFile(h, &text[0], (DWORD)sz.QuadPart, &rd, nullptr)) text.resize(rd);
    }
    CloseHandle(h);
    if (text.empty()) return {};
    Json j;
    if (!Json::parse(text, j)) return {};
    const Json* data = j.find("tiny_storage_data");
    if (!data || !data->isObject()) return {};
    const Json* idv = data->find("aha.device.device_id");
    if (!idv || !idv->isString() || idv->asString().empty()) return {};
    std::string plain, err;
    if (!decryptStorageValue(idv->asString(), plain, err)) {
        LOG_W("AHA device_id 解密失败: %s", err.c_str());
        return {};
    }
    Json dj;
    if (!Json::parse(plain, dj)) return {};
    const Json* did = dj.find("device_id_str");
    if (did && did->isString() && !did->asString().empty()) return did->asString();
    return {};
}

std::string Storage::readDeviceId(const std::string& userDir) {
    std::string mp = userDir.substr(0, userDir.rfind("\\User")) + "\\machineid";
    HANDLE mh = CreateFileW(widenPath(mp).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (mh != INVALID_HANDLE_VALUE) {
        char buf[128] = { 0 };
        DWORD rd = 0;
        ReadFile(mh, buf, sizeof(buf) - 1, &rd, nullptr);
        CloseHandle(mh);
        std::string local(buf);
        local.erase(std::remove_if(local.begin(), local.end(), [](unsigned char c) { return isspace(c); }), local.end());
        if (!local.empty()) return local;
    }
    std::string sp = userDir + "\\globalStorage\\storage.json";
    HANDLE h = CreateFileW(widenPath(sp).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart >= 64 * 1024 * 1024) {
        CloseHandle(h);
        return {};
    }
    std::string text((size_t)sz.QuadPart, 0);
    DWORD rd = 0;
    ReadFile(h, &text[0], (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(h);
    Json j;
    if (!Json::parse(text, j)) return {};
    const Json* v = j.find("telemetry.devDeviceId");
    if (v && v->isString() && !v->asString().empty()) return v->asString();
    return {};
}

static bool deriveKeyIv(const uint8_t* randomBytes, bool privateSalt, uint8_t key[16], uint8_t iv[16]) {
    std::vector<uint8_t> salt(64);
    for (int i = 0; i < 64; ++i)
        salt[i] = privateSalt ? (SALT_C[i] ^ SALT_D[i]) : (SALT_A[i] ^ SALT_B[i]);
    std::vector<uint8_t> h = crypto::sha512(randomBytes, 32);
    std::vector<uint8_t> f = crypto::sha512Concat({ h, salt });
    memcpy(key, f.data(), 16);
    memcpy(iv, f.data() + 16, 16);
    return true;
}

bool Storage::decryptStorageValue(const std::string& base64Value, std::string& plain, std::string& err) {
    std::vector<uint8_t> buf;
    if (!crypto::base64Decode(base64Value, buf) || buf.size() < 38 + 16) {
        err = "tc base64 解码失败或长度不足";
        return false;
    }
    // [6B Header][32B RandomBytes][N EncryptedData]
    const uint8_t* header = buf.data();
    const uint8_t* rnd = buf.data() + 6;
    const uint8_t* enc = buf.data() + 38;
    size_t encLen = buf.size() - 38;

    // 判定类型：tc 05 10 00 00 = AES；12 39 20 20 02 03 = AES_PRIVATE
    bool isAes = header[0] == 0x74 && header[1] == 0x63 && header[2] == 0x05 &&
                 header[3] == 0x10 && header[4] == 0x00 && header[5] == 0x00;
    bool isPriv = header[0] == 18 && header[1] == 57 && header[2] == 32 &&
                  header[3] == 32 && header[4] == 2 && header[5] == 3;
    if (!isAes && !isPriv) {
        char hx[32];
        snprintf(hx, sizeof(hx), "%02x%02x%02x%02x%02x%02x", header[0], header[1], header[2], header[3], header[4], header[5]);
        err = std::string("未知加密类型 header=") + hx;
        return false;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool usePriv = isPriv ? (attempt == 0) : (attempt == 1);
        uint8_t key[16], iv[16];
        deriveKeyIv(rnd, usePriv, key, iv);
        std::vector<uint8_t> dec;
        if (!crypto::aes128CbcDecrypt(key, iv, enc, encLen, dec) || dec.size() <= 64) continue;
        std::vector<uint8_t> storedHash(dec.begin(), dec.begin() + 64);
        std::vector<uint8_t> body(dec.begin() + 64, dec.end());
        if (!crypto::pkcs7Unpad(body)) continue;
        std::vector<uint8_t> calc = crypto::sha512(body.data(), body.size());
        if (calc == storedHash) {
            plain.assign(body.begin(), body.end());
            return true;
        }
    }
    err = "哈希校验失败（两种盐均不匹配）";
    return false;
}

static std::string firstStr(const Json& j, std::initializer_list<const char*> keys) {
    for (auto k : keys) {
        const Json* v = j.find(k);
        if (v && v->isString() && !v->asString().empty()) return v->asString();
    }
    return "";
}

long long Storage::parseExpiry(const std::string& raw) {
    if (raw.empty()) return 0;
    // 纯数字
    char* end = nullptr;
    double v = strtod(raw.c_str(), &end);
    if (end && *end == '\0' && v > 0) {
        if (v > 1e12) v /= 1000.0; // 毫秒
        return (long long)v;
    }
    // ISO 8601: 2026-09-16T12:00:00Z / +08:00
    struct tm tmv{};
    int y, mo, d, h, mi;
    double s;
    if (sscanf(raw.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &s) == 6) {
        int sec = (int)s;
        // 处理时区后缀
        int tzOffsetSec = 0;
        size_t zp = raw.find('Z');
        if (zp == std::string::npos) {
            size_t pp = raw.find('+', 10);
            size_t mp = raw.rfind('-');
            size_t tp = pp != std::string::npos ? pp : (mp != std::string::npos && mp > 10 ? mp : std::string::npos);
            if (tp != std::string::npos) {
                int th = 0, tm2 = 0;
                if (sscanf(raw.c_str() + tp + 1, "%d:%d", &th, &tm2) == 2) {
                    tzOffsetSec = th * 3600 + tm2 * 60;
                    if (raw[tp] == '-') tzOffsetSec = -tzOffsetSec;
                }
            }
        }
        tmv.tm_year = y - 1900;
        tmv.tm_mon = mo - 1;
        tmv.tm_mday = d;
        tmv.tm_hour = h;
        tmv.tm_min = mi;
        tmv.tm_sec = sec;
#ifdef _WIN32
        time_t t = _mkgmtime(&tmv);
#else
        time_t t = timegm(&tmv);
#endif
        if (t > 0) return (long long)t - tzOffsetSec;
    }
    return 0;
}

static void fillPsd(AuthData& a, const Json& j) {
    a.webId = firstStr(j, { "webId", "web_id", "WebId" });
    a.bizUserId = firstStr(j, { "bizUserId", "biz_user_id", "BizUserId" });
    a.userUniqueId = firstStr(j, { "userUniqueId", "user_unique_id", "UserUniqueId" });
    a.scope = firstStr(j, { "scope", "Scope" });
    a.tenant = firstStr(j, { "tenant", "Tenant" });
    a.region = firstStr(j, { "region", "Region" });
    a.aiRegion = firstStr(j, { "aiRegion", "AIRegion" });
    a.appLanguage = firstStr(j, { "appLanguage", "AppLanguage" });
    a.appVersion = firstStr(j, { "appVersion", "AppVersion" });
    a.userRegion = firstStr(j, { "userRegion", "UserRegion" });
    a.userIdentity = firstStr(j, { "userIdentity", "UserIdentity" });
    // 嵌套对象形态
    const Json* psd = j.find("providerSpecificData");
    if (!psd) psd = j.find("commonParams");
    if (!psd) psd = j.find("common_params");
    if (psd && psd->isObject()) fillPsd(a, *psd);
}

bool Storage::readAuth(const std::string& userDir, AuthData& out, std::string& err) {
    std::string sp = userDir + "\\globalStorage\\storage.json";
    HANDLE h = CreateFileW(widenPath(sp).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "无法打开 " + sp + "（错误码 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        err = "storage.json 尺寸异常";
        return false;
    }
    std::string text((size_t)sz.QuadPart, 0);
    DWORD rd = 0;
    BOOL ok = ReadFile(h, &text[0], (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(h);
    if (!ok) {
        err = "storage.json 读取失败";
        return false;
    }
    Json root;
    if (!Json::parse(text, root, &err)) {
        err = "storage.json 不是合法 JSON: " + err;
        return false;
    }
    const Json* encVal = root.find("iCubeAuthInfo://icube.cloudide");
    if (!encVal || !encVal->isString() || encVal->asString().empty()) {
        err = "storage.json 缺少 iCubeAuthInfo://icube.cloudide";
        return false;
    }
    std::string authText = encVal->asString();
    Json auth;
    // 明文 JSON 兜底（部分版本/国际版）
    size_t firstNonWs = authText.find_first_not_of(" \t\r\n");
    if (firstNonWs != std::string::npos && authText[firstNonWs] == '{') {
        if (!Json::parse(authText, auth, &err)) {
            err = "iCubeAuthInfo 明文 JSON 解析失败: " + err;
            return false;
        }
    } else {
        std::string plain;
        if (!decryptStorageValue(authText, plain, err)) return false;
        if (!Json::parse(plain, auth, &err)) {
            err = "tc 解密成功但 JSON 解析失败: " + err;
            return false;
        }
    }
    out.raw = auth;
    out.accessToken = firstStr(auth, { "token", "accessToken", "AccessToken", "access_token" });
    out.refreshToken = firstStr(auth, { "refreshToken", "RefreshToken", "refresh_token" });
    out.userId = firstStr(auth, { "userId", "UserID", "userID", "user_id", "uid" });
    out.expiredRaw = firstStr(auth, { "expiredAt", "TokenExpireAt", "tokenExpireAt", "expireAt" });
    out.expiredTs = parseExpiry(out.expiredRaw);
    out.host = firstStr(auth, { "host", "Host" });
    out.clientId = firstStr(auth, { "clientID", "ClientID", "clientId" });
    fillPsd(out, auth);
    if (out.accessToken.empty()) {
        err = "解密结果中没有 token 字段";
        return false;
    }
    return true;
}

// ===== Trae 安装目录与 IDE 版本探测 =====

static bool probeFileExists(const std::string& utf8Path) {
    DWORD a = GetFileAttributesW(widenPath(utf8Path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string probeReadText(const std::string& utf8Path) {
    HANDLE h = CreateFileW(widenPath(utf8Path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string text;
    if (sz.QuadPart > 0 && sz.QuadPart < 16 * 1024 * 1024) {
        text.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        if (!ReadFile(h, &text[0], (DWORD)sz.QuadPart, &rd, nullptr)) text.clear();
    }
    CloseHandle(h);
    return text;
}

// 遍历某个 Uninstall 根键（HKCU/HKLM、64/32 视图），按 DisplayName 匹配 Trae，
// 返回经 product.json 校验的 InstallLocation。
static std::string scanUninstall(HKEY root, REGSAM wow) {
    HKEY base{};
    if (RegOpenKeyExW(root,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                      0, KEY_READ | wow, &base) != ERROR_SUCCESS)
        return {};
    std::string found;
    DWORD idx = 0;
    for (;;) {
        wchar_t sub[256] = { 0 };
        DWORD subLen = 256;
        LONG rc = RegEnumKeyExW(base, idx++, sub, &subLen, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) break;
        HKEY k{};
        if (RegOpenKeyExW(base, sub, 0, KEY_READ | wow, &k) != ERROR_SUCCESS) continue;
        auto readStr = [&](const wchar_t* name) -> std::string {
            DWORD type = 0, len = 0;
            if (RegQueryValueExW(k, name, nullptr, &type, nullptr, &len) != ERROR_SUCCESS ||
                (type != REG_SZ && type != REG_EXPAND_SZ) || len == 0)
                return {};
            std::wstring w(len / 2 + 1, L'\0');
            DWORD n = (DWORD)w.size() * sizeof(wchar_t);
            if (RegQueryValueExW(k, name, nullptr, nullptr, (LPBYTE)&w[0], &n) != ERROR_SUCCESS)
                return {};
            w.resize(n / sizeof(wchar_t));
            while (!w.empty() && w.back() == L'\0') w.pop_back();
            if (type == REG_EXPAND_SZ) {
                wchar_t exp[MAX_PATH * 2] = { 0 };
                DWORD en = ExpandEnvironmentStringsW(w.c_str(), exp, MAX_PATH * 2);
                if (en > 0 && en < MAX_PATH * 2) w = exp;
            }
            int cn = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string out(cn, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], cn, nullptr, nullptr);
            return out;
        };
        std::string name = readStr(L"DisplayName");
        std::string loc = readStr(L"InstallLocation");
        RegCloseKey(k);
        std::string low = name;
        for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
        // 覆盖 TraeCode CN (User) / Trae CN；排除 SOLO / Work。
        if (low.find("trae") != std::string::npos &&
            low.find("solo") == std::string::npos &&
            low.find("work") == std::string::npos &&
            !loc.empty()) {
            while (!loc.empty() && (loc.back() == '\\' || loc.back() == '/')) loc.pop_back();
            if (probeFileExists(loc + "\\resources\\app\\product.json")) { found = loc; break; }
        }
    }
    RegCloseKey(base);
    return found;
}

std::string Storage::detectInstallDir() {
    const HKEY roots[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    const REGSAM views[2] = { 0, KEY_WOW64_32KEY };
    for (HKEY r : roots)
        for (REGSAM v : views) {
            std::string d = scanUninstall(r, v);
            if (!d.empty()) return d;
        }
    // 兜底：常见安装路径
    wchar_t localApp[MAX_PATH] = { 0 };
    std::string la;
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp) == S_OK) {
        int n = WideCharToMultiByte(CP_UTF8, 0, localApp, -1, nullptr, 0, nullptr, nullptr);
        la.resize(n - 1);
        WideCharToMultiByte(CP_UTF8, 0, localApp, -1, &la[0], n, nullptr, nullptr);
    }
    const std::string fallbacks[3] = {
        la + "\\Programs\\Trae CN",
        "C:\\Program Files\\Trae CN",
        "C:\\Program Files (x86)\\Trae CN",
    };
    for (const auto& fb : fallbacks) {
        if (!fb.empty() && probeFileExists(fb + "\\resources\\app\\product.json")) return fb;
    }
    return {};
}

bool Storage::detectIdeVersion(std::string& ver, std::string& code) {
    std::string dir = detectInstallDir();
    if (dir.empty()) return false;
    std::string text = probeReadText(dir + "\\resources\\app\\product.json");
    if (text.empty()) return false;
    Json j;
    if (!Json::parse(text, j)) return false;
    const Json* av = j.find("appVersion");
    if (!av || !av->isString() || av->asString().empty()) return false;
    ver = av->asString();
    // date 形如 2026-09-16T09:37:29.620Z，取前 10 位去横线得到版本码 20260916。
    const Json* dt = j.find("date");
    if (dt && dt->isString() && dt->asString().size() >= 10) {
        std::string d = dt->asString().substr(0, 10);
        std::string digits;
        for (char ch : d) if (ch >= '0' && ch <= '9') digits.push_back(ch);
        if (digits.size() == 8) code = digits;
    }
    return !ver.empty();
}
