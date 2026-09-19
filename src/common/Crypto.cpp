// Crypto.cpp - Windows CNG 实现
#include "common/Crypto.h"
#include <windows.h>
#include <bcrypt.h>
#include <mutex>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace {

struct AlgoHolder {
    BCRYPT_ALG_HANDLE sha512 = nullptr;
    BCRYPT_ALG_HANDLE aes = nullptr;
    BCRYPT_ALG_HANDLE rng = nullptr;
    bool ok = false;
    AlgoHolder() {
        ok = BCryptOpenAlgorithmProvider(&sha512, BCRYPT_SHA512_ALGORITHM, nullptr, 0) == STATUS_SUCCESS &&
             BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0) == STATUS_SUCCESS &&
             BCryptOpenAlgorithmProvider(&rng, BCRYPT_RNG_ALGORITHM, nullptr, 0) == STATUS_SUCCESS;
    }
    ~AlgoHolder() {
        if (sha512) BCryptCloseAlgorithmProvider(sha512, 0);
        if (aes) BCryptCloseAlgorithmProvider(aes, 0);
        if (rng) BCryptCloseAlgorithmProvider(rng, 0);
    }
};
AlgoHolder& algos() {
    static AlgoHolder a;
    return a;
}

} // namespace

namespace crypto {

std::vector<uint8_t> sha512(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out(64);
    BCRYPT_HASH_HANDLE h = nullptr;
    DWORD cbHash = 0, cbObj = 0, cbData = 0;
    if (BCryptGetProperty(algos().sha512, BCRYPT_HASH_LENGTH, (PUCHAR)&cbHash, sizeof(cbHash), &cbData, 0) != STATUS_SUCCESS)
        throw std::runtime_error("sha512: getprop");
    out.resize(cbHash);
    BCryptGetProperty(algos().sha512, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbObj, sizeof(cbObj), &cbData, 0);
    std::vector<uint8_t> obj(cbObj);
    if (BCryptCreateHash(algos().sha512, &h, obj.data(), cbObj, nullptr, 0, 0) != STATUS_SUCCESS)
        throw std::runtime_error("sha512: create");
    if (len) BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0);
    BCryptFinishHash(h, out.data(), cbHash, 0);
    BCryptDestroyHash(h);
    return out;
}

std::vector<uint8_t> sha512(const std::string& s) {
    return sha512((const uint8_t*)s.data(), s.size());
}

std::vector<uint8_t> sha512Concat(const std::vector<std::vector<uint8_t>>& parts) {
    std::vector<uint8_t> out(64);
    BCRYPT_HASH_HANDLE h = nullptr;
    DWORD cbObj = 0, cbData = 0, cbHash = 64;
    BCryptGetProperty(algos().sha512, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbObj, sizeof(cbObj), &cbData, 0);
    std::vector<uint8_t> obj(cbObj);
    if (BCryptCreateHash(algos().sha512, &h, obj.data(), cbObj, nullptr, 0, 0) != STATUS_SUCCESS)
        throw std::runtime_error("sha512: create");
    for (const auto& part : parts)
        if (!part.empty()) BCryptHashData(h, (PUCHAR)part.data(), (ULONG)part.size(), 0);
    BCryptFinishHash(h, out.data(), cbHash, 0);
    BCryptDestroyHash(h);
    return out;
}

bool aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t* in, size_t len, std::vector<uint8_t>& out) {
    if (len == 0 || len % 16 != 0) return false;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    DWORD cbData = 0;
    if (BCryptGenerateSymmetricKey(algos().aes, &hKey, nullptr, 0, (PUCHAR)key, 16, 0) != STATUS_SUCCESS)
        return false;
    // 链式模式 CBC
    BCryptSetProperty(algos().aes, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                      sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    out.resize(len);
    NTSTATUS st = BCryptDecrypt(hKey, (PUCHAR)in, (ULONG)len, nullptr, (PUCHAR)iv, 16,
                                out.data(), (ULONG)len, &cbData, 0);
    BCryptDestroyKey(hKey);
    if (st != STATUS_SUCCESS) return false;
    out.resize(cbData);
    return true;
}

bool pkcs7Unpad(std::vector<uint8_t>& data, size_t block) {
    if (data.empty() || data.size() % block != 0) return false;
    uint8_t pad = data.back();
    if (pad < 1 || pad > block || pad > data.size()) return false;
    for (size_t i = data.size() - pad; i < data.size(); ++i)
        if (data[i] != pad) return false;
    data.resize(data.size() - pad);
    return true;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += B64[v & 63];
    }
    if (len - i == 1) {
        uint32_t v = data[i] << 16;
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += "==";
    } else if (len - i == 2) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}
std::string base64Encode(const std::vector<uint8_t>& v) { return base64Encode(v.data(), v.size()); }

bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    static int8_t table[256];
    static bool init = false;
    if (!init) {
        memset(table, -1, sizeof(table));
        for (int i = 0; i < 64; ++i) table[(uint8_t)B64[i]] = (int8_t)i;
        init = true;
    }
    out.clear();
    out.reserve(in.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c == '=') break;
        int8_t v = table[(uint8_t)c];
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    return true;
}

std::vector<uint8_t> randomBytes(size_t n) {
    std::vector<uint8_t> v(n);
    if (n) BCryptGenRandom(nullptr, v.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return v;
}

std::string genUuid() {
    auto b = randomBytes(16);
    b[6] = (b[6] & 0x0F) | 0x40; // v4
    b[8] = (b[8] & 0x3F) | 0x80;
    char buf[40];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}

std::string genApiKey() {
    auto b = randomBytes(12);
    char buf[40] = "sk-trae-";
    for (int i = 0; i < 12; ++i)
        snprintf(buf + 8 + i * 2, 3, "%02x", b[i]);
    return buf;
}

std::string sha512Hex(const std::string& s) {
    auto h = sha512(s);
    std::string out;
    out.reserve(128);
    char buf[3];
    for (uint8_t c : h) {
        snprintf(buf, sizeof(buf), "%02x", c);
        out += buf;
    }
    return out;
}

} // namespace crypto
