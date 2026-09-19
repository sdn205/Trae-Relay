// Crypto.h - CNG (bcrypt.dll) 封装：SHA-512 / AES-128-CBC / PKCS7 / Base64 / 随机数
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace crypto {

// SHA-512
std::vector<uint8_t> sha512(const uint8_t* data, size_t len);
std::vector<uint8_t> sha512(const std::string& s);
// 多段拼接哈希
std::vector<uint8_t> sha512Concat(const std::vector<std::vector<uint8_t>>& parts);

// AES-128-CBC 解密（输入长度必须为 16 的倍数）
bool aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t* in, size_t len, std::vector<uint8_t>& out);

// PKCS7 去填充（block=16），失败返回 false 且不修改 data
bool pkcs7Unpad(std::vector<uint8_t>& data, size_t block = 16);

std::string base64Encode(const uint8_t* data, size_t len);
std::string base64Encode(const std::vector<uint8_t>& v);
bool base64Decode(const std::string& in, std::vector<uint8_t>& out);

std::vector<uint8_t> randomBytes(size_t n);

// UUID v4（小写，带连字符）
std::string genUuid();

// sk-trae- 前缀 + 24 位随机 hex
std::string genApiKey();

// SHA-512 hex（64 字符小写）
std::string sha512Hex(const std::string& s);

} // namespace crypto
