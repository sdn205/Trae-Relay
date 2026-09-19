// Http.h - 轻量 HTTP/HTTPS 客户端（WinSock2 + schannel），支持 SSE 流式读取
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace http {

struct Headers {
    std::vector<std::pair<std::string, std::string>> v;
    void set(const std::string& k, const std::string& val);   // 覆盖同名（忽略大小写）
    void add(const std::string& k, const std::string& val);
    const std::string* get(const std::string& k) const;       // 找不到返回 nullptr
    void remove(const std::string& k);
};

struct Response {
    int status = 0;
    Headers headers;
    std::string body;
    std::string error;        // 非空 = 网络/协议错误
    bool ok() const { return status >= 200 && status < 300 && error.empty(); }
};

// SSE / 长连接流式响应
struct Stream {
    int status = 0;
    Headers headers;
    std::string error;
    // 读一行（不含行尾 CRLF）。返回 false 表示流结束/超时/错误。
    bool readLine(std::string& line);
    void close();
    ~Stream() { close(); }

    // 内部
    void* impl = nullptr;
};

// 同步请求（响应整体读回）
Response send(const std::string& method, const std::string& url,
              const Headers& headers, const std::string& body,
              int timeoutMs = 120000, int connectTimeoutMs = 15000);

// 打开流式请求（SSE）。firstTimeoutMs：等待首字节的超时；readTimeoutMs：之后的每次读超时。
std::unique_ptr<Stream> openStream(const std::string& method, const std::string& url,
                                   const Headers& headers, const std::string& body,
                                   int firstTimeoutMs = 120000, int readTimeoutMs = 300000);

// URL 解析：scheme, host, port, path
bool parseUrl(const std::string& url, std::string& scheme, std::string& host,
              int& port, std::string& path);

} // namespace http
