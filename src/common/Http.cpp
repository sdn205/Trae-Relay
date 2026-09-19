// Http.cpp - WinHTTP 实现（系统级 schannel TLS，chunked 自动解码，稳健可靠）
#include "common/Http.h"
#include "Version.h"
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "winhttp.lib")

namespace http {

// ---------- Headers ----------
static std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
void Headers::set(const std::string& k, const std::string& val) {
    remove(k);
    v.emplace_back(k, val);
}
void Headers::add(const std::string& k, const std::string& val) { v.emplace_back(k, val); }
const std::string* Headers::get(const std::string& k) const {
    std::string lk = lower(k);
    for (auto& p : v)
        if (lower(p.first) == lk) return &p.second;
    return nullptr;
}
void Headers::remove(const std::string& k) {
    std::string lk = lower(k);
    for (size_t i = 0; i < v.size();) {
        if (lower(v[i].first) == lk) v.erase(v.begin() + i);
        else ++i;
    }
}

bool parseUrl(const std::string& url, std::string& scheme, std::string& host, int& port, std::string& path) {
    size_t ps = url.find("://");
    if (ps == std::string::npos) return false;
    scheme = lower(url.substr(0, ps));
    size_t hs = ps + 3;
    size_t pe = url.find('/', hs);
    std::string hostport = pe == std::string::npos ? url.substr(hs) : url.substr(hs, pe - hs);
    path = pe == std::string::npos ? "/" : url.substr(pe);
    port = scheme == "https" ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t c = hostport.rfind(':');
    if (c != std::string::npos && hostport.find(']') == std::string::npos) {
        host = hostport.substr(0, c);
        port = atoi(hostport.c_str() + c + 1);
    } else {
        host = hostport;
    }
    return !host.empty();
}

static std::wstring toWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// ---------- 共享请求执行 ----------
struct WinHttpCtx {
    HINTERNET hSession = nullptr, hConnect = nullptr, hRequest = nullptr;
    int status = 0;
    Headers headers;
    std::string plainBuf; // 解码后字节缓冲（readLine 消费）
    bool eof = false;
    std::string err;
    bool gotFirst = false;
    int firstTimeoutMs = 120000, readTimeoutMs = 300000;

    ~WinHttpCtx() { closeAll(); }

    void closeAll() {
        if (hRequest) { WinHttpCloseHandle(hRequest); hRequest = nullptr; }
        if (hConnect) { WinHttpCloseHandle(hConnect); hConnect = nullptr; }
        if (hSession) { WinHttpCloseHandle(hSession); hSession = nullptr; }
    }

    bool open(const std::string& method, const std::string& url, const Headers& hdrs,
              const std::string& body, int connectTimeoutMs) {
        std::string scheme, host, path;
        int port;
        if (!parseUrl(url, scheme, host, port, path)) {
            err = "URL 无效: " + url;
            return false;
        }
        hSession = WinHttpOpen(L"TraeRelay/" TRAERELAY_VERSION_W, WINHTTP_ACCESS_TYPE_NO_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { err = "WinHttpOpen 失败: " + std::to_string(GetLastError()); return false; }
        WinHttpSetTimeouts(hSession, 15000, 15000, 30000, 60000);
        hConnect = WinHttpConnect(hSession, toWide(host).c_str(), (INTERNET_PORT)port, 0);
        if (!hConnect) { err = "连接失败: " + std::to_string(GetLastError()); return false; }
        DWORD flags = scheme == "https" ? WINHTTP_FLAG_SECURE : 0;
        hRequest = WinHttpOpenRequest(hConnect, toWide(method).c_str(), toWide(path).c_str(),
                                      nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      flags);
        if (!hRequest) { err = "OpenRequest 失败: " + std::to_string(GetLastError()); return false; }
        (void)connectTimeoutMs;
        // 请求头
        std::string hd;
        bool hasCL = false;
        for (auto& p : hdrs.v) {
            std::string lk = lower(p.first);
            if (lk == "content-length") hasCL = true;
            hd += p.first + ": " + p.second + "\r\n";
        }
        if (!hasCL && (method == "POST" || method == "PUT" || method == "PATCH"))
            hd += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        std::wstring whd = toWide(hd);
        if (!WinHttpSendRequest(hRequest, hd.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : whd.c_str(),
                                (DWORD)hd.size(), (LPVOID)(body.empty() ? nullptr : body.data()),
                                (DWORD)body.size(), (DWORD)body.size(), 0)) {
            err = "发送失败: " + std::to_string(GetLastError());
            return false;
        }
        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            err = "接收响应失败: " + std::to_string(GetLastError());
            return false;
        }
        // 状态码
        DWORD sc = 0, sz = sizeof(sc);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &sc, &sz, WINHTTP_NO_HEADER_INDEX);
        status = (int)sc;
        // 响应头（原始行）
        sz = 0;
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            WINHTTP_NO_OUTPUT_BUFFER, &sz, WINHTTP_NO_HEADER_INDEX);
        if (sz > 0) {
            std::wstring wraw(sz / sizeof(wchar_t) + 1, 0);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &wraw[0], &sz, WINHTTP_NO_HEADER_INDEX)) {
                std::string raw;
                int n = WideCharToMultiByte(CP_UTF8, 0, wraw.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (n > 1) {
                    raw.resize(n - 1);
                    WideCharToMultiByte(CP_UTF8, 0, wraw.c_str(), -1, &raw[0], n, nullptr, nullptr);
                }
                // 解析行
                size_t start = 0;
                bool first = true;
                while (start < raw.size()) {
                    size_t e = raw.find("\r\n", start);
                    std::string line = raw.substr(start, e == std::string::npos ? std::string::npos : e - start);
                    if (!first) {
                        size_t cp = line.find(':');
                        if (cp != std::string::npos) {
                            std::string k = line.substr(0, cp);
                            std::string val = line.substr(cp + 1);
                            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
                            headers.add(k, val);
                        }
                    }
                    first = false;
                    if (e == std::string::npos) break;
                    start = e + 2;
                }
            }
        }
        return true;
    }

    // 读取解码后的字节数据（追加到 plainBuf）；返回 false = EOF/错误
    bool pump() {
        if (eof) return false;
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
            err = "查询数据失败: " + std::to_string(GetLastError());
            eof = true;
            return false;
        }
        if (avail == 0) {
            eof = true;
            return false;
        }
        std::vector<char> tmp(avail);
        DWORD rd = 0;
        if (!WinHttpReadData(hRequest, tmp.data(), avail, &rd)) {
            err = "读取失败: " + std::to_string(GetLastError());
            eof = true;
            return false;
        }
        if (rd == 0) {
            eof = true;
            return false;
        }
        plainBuf.append(tmp.data(), rd);
        return true;
    }

    int currentTimeout() const { return gotFirst ? readTimeoutMs : firstTimeoutMs; }

    // 读一行（含跨块）；false = 流结束
    bool readLine(std::string& line) {
        // 设置读超时（首次/后续）
        WinHttpSetTimeouts(hRequest, 0, 0, currentTimeout(), currentTimeout());
        while (true) {
            size_t pos = plainBuf.find('\n');
            if (pos != std::string::npos) {
                line = plainBuf.substr(0, pos);
                plainBuf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                gotFirst = true;
                return true;
            }
            if (!pump()) return false;
        }
    }
};

Response send(const std::string& method, const std::string& url,
              const Headers& headers, const std::string& body,
              int timeoutMs, int connectTimeoutMs) {
    Response resp;
    WinHttpCtx ctx;
    ctx.firstTimeoutMs = timeoutMs;
    ctx.readTimeoutMs = timeoutMs;
    if (!ctx.open(method, url, headers, body, connectTimeoutMs)) {
        resp.error = ctx.err;
        resp.status = ctx.status;
        return resp;
    }
    resp.status = ctx.status;
    resp.headers = ctx.headers;
    // 读全部正文
    while (ctx.pump()) {
        resp.body += ctx.plainBuf;
        ctx.plainBuf.clear();
        if (resp.body.size() > 512ULL * 1024 * 1024) break;
    }
    if (!resp.body.empty() || ctx.eof) {
        // 正常读完或 EOF
    } else if (!ctx.err.empty()) {
        resp.error = ctx.err;
    }
    ctx.closeAll();
    return resp;
}

struct StreamImpl {
    WinHttpCtx* ctx = nullptr;
};

std::unique_ptr<Stream> openStream(const std::string& method, const std::string& url,
                                   const Headers& headers, const std::string& body,
                                   int firstTimeoutMs, int readTimeoutMs) {
    auto st = std::make_unique<Stream>();
    auto impl = new StreamImpl();
    impl->ctx = new WinHttpCtx();
    st->impl = impl;
    WinHttpCtx& c = *impl->ctx;
    c.firstTimeoutMs = firstTimeoutMs;
    c.readTimeoutMs = readTimeoutMs;
    if (!c.open(method, url, headers, body, 15000)) {
        st->error = c.err;
        st->status = c.status;
        delete impl->ctx;
        impl->ctx = nullptr;
        delete impl;
        st->impl = nullptr;
    } else {
        st->status = c.status;
        st->headers = c.headers;
    }
    return st;
}

bool Stream::readLine(std::string& line) {
    if (!impl) return false;
    auto* p = (StreamImpl*)impl;
    if (!p->ctx) return false;
    return p->ctx->readLine(line);
}

void Stream::close() {
    if (impl) {
        auto* p = (StreamImpl*)impl;
        if (p->ctx) delete p->ctx;
        delete p;
        impl = nullptr;
    }
}

} // namespace http
