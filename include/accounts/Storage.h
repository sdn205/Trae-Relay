// Storage.h - Trae CN storage.json 读取 + tc 解密 + 明文兜底 + 账号凭证模型
#pragma once
#include <string>
#include <vector>
#include "common/Json.h"

// 返回 TraeRelay.exe 所在目录（无尾部分隔符，UTF-8）。
// 配置与日志均放在该目录，做到程序目录级可移植。
std::string exeDir();

// 一台机器上可发现的账号来源目录（多版本共存）
struct TraeEdition {
    std::string id;         // cn / solo / sg
    std::string userDir;    // %APPDATA%\Trae CN\User
    std::string label;
};

struct AuthData {
    std::string accessToken;   // token
    std::string refreshToken;
    std::string userId;
    std::string expiredRaw;    // expiredAt / TokenExpireAt 原串
    long long expiredTs = 0;   // 归一为秒级时间戳；0 = 未知
    std::string host;          // 账号绑定的 API host（可能为空）
    std::string clientId;
    // psd (providerSpecificData) 常用字段
    std::string webId, bizUserId, userUniqueId, scope, tenant, region, aiRegion,
        appLanguage, appVersion, userRegion, userIdentity;
    Json raw;                  // 解密后的完整 JSON
};

class Storage {
public:
    // 发现本机全部 Trae 数据目录
    static std::vector<TraeEdition> discoverEditions();

    // 读取 machineId：优先 storage.json 的 telemetry.machineId，其次 machineid 文件
    static std::string readMachineId(const std::string& userDir);
    // 读取 AHA/TTNet 设备 ID（aha\TinyStorage 的加密 aha.device.device_id，
    // 明文 JSON 的 device_id_str，数字形态）。发积分类商业接口（签到 claim）
    // 会校验该 ID 是否为账号注册过的设备，缺失或不认识会被 9074 软拒。
    // 失败返回空串。
    static std::string readAhaDeviceId(const std::string& userDir);
    // 兜底设备 ID：machineid 文件 / storage.json 的 telemetry.devDeviceId
    static std::string readDeviceId(const std::string& userDir);

    // 读取并解密凭证；失败返回 false 并填充 err
    static bool readAuth(const std::string& userDir, AuthData& out, std::string& err);

    // tc 格式解密单值（base64 → 明文 JSON 字符串）
    static bool decryptStorageValue(const std::string& base64Value, std::string& plain, std::string& err);

    // expiredRaw → 秒级时间戳（>1e12 视为毫秒；ISO 8601 亦可）
    static long long parseExpiry(const std::string& raw);

    // 发现本机 Trae CN 安装目录（注册表 Uninstall InstallLocation，兜底常见路径）。
    // 失败返回空串。
    static std::string detectInstallDir();
    // 从安装目录 resources/app/product.json 探测 IDE 版本头：
    // ver  <- appVersion（x-ide-version）；code <- date 的 YYYYMMDD（x-ide-version-code）。
    static bool detectIdeVersion(std::string& ver, std::string& code);
};
