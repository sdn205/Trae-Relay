// HttpServer.cpp
#include "common/HttpServer.h"
#include "common/Log.h"
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <memory>

#pragma comment(lib, "ws2_32.lib")

namespace {

struct WsaInit {
    bool ok = false;
    WsaInit() {
        WSADATA wd;
        ok = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    }
    ~WsaInit() { if (ok) WSACleanup(); }
};

constexpr size_t kMaxHeader = 64 * 1024;
constexpr size_t kMaxBody = 512ULL * 1024 * 1024;
// 请求级并发上限（仅统计正在处理的请求；keep-alive 空闲连接不占槽）。
constexpr int kMaxConcurrent = 64;

struct ServerImpl;

struct ConnImpl : public std::enable_shared_from_this<ConnImpl> {
    SOCKET s = INVALID_SOCKET;
    std::shared_ptr<ServerImpl> server;
    bool chunked = false;
    bool keepAlive = true;
    std::string lineBuf; // 头部行缓冲（每连接独立，避免线程局部存储）
    std::atomic<bool> m_goneFlag{ false };

    ConnImpl(SOCKET sock, std::shared_ptr<ServerImpl> sv) : s(sock), server(std::move(sv)) {}
    ~ConnImpl() {
        if (s != INVALID_SOCKET) closesocket(s);
    }

    bool sendAll(const char* data, size_t len) {
        size_t off = 0;
        while (off < len) {
            int n = ::send(s, data + off, (int)(len - off), 0);
            if (n == SOCKET_ERROR) { m_goneFlag = true; return false; }
            off += n;
        }
        return true;
    }

    // 服务停机时强制打断阻塞中的 recv/send（只 shutdown 不 closesocket，
    // 描述符仍由工作线程最终关闭，避免 double close / fd 复用）
    void shutdown() {
        if (s != INVALID_SOCKET) ::shutdown(s, SD_BOTH);
    }
};

struct ServerImpl : public std::enable_shared_from_this<ServerImpl> {
    SOCKET listen = INVALID_SOCKET;
    std::thread acceptThread;
    std::atomic<bool> running{ false };
    std::atomic<int> active{ 0 }; // 正在处理中的请求数（不含 keep-alive 空闲）
    HttpHandler handler;

    std::mutex connMtx;
    std::vector<std::shared_ptr<ConnImpl>> conns; // 在途连接（停机时统一 shutdown）

    void registerConn(const std::shared_ptr<ConnImpl>& c) {
        std::lock_guard<std::mutex> lk(connMtx);
        conns.push_back(c);
    }
    void unregisterConn(const std::shared_ptr<ConnImpl>& c) {
        std::lock_guard<std::mutex> lk(connMtx);
        conns.erase(std::find(conns.begin(), conns.end(), c), conns.end());
    }
};

// 读一行（\r\n 结尾）
bool readLineSock(ConnImpl& c, std::string& line, int timeoutMs) {
    while (true) {
        size_t pos = c.lineBuf.find("\r\n");
        if (pos != std::string::npos) {
            line = c.lineBuf.substr(0, pos);
            c.lineBuf.erase(0, pos + 2);
            return true;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(c.s, &fds);
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        int r = select(0, &fds, nullptr, nullptr, &tv);
        if (r <= 0) return false;
        char tmp[8192];
        int n = ::recv(c.s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        c.lineBuf.append(tmp, n);
        if (c.lineBuf.size() > kMaxHeader) return false;
    }
}

void sendStatus(ConnImpl& c, int status, const std::string& statusText) {
    char buf[64];
    snprintf(buf, sizeof(buf), "HTTP/1.1 %d %s\r\n", status, statusText.c_str());
    c.sendAll(buf, strlen(buf));
}

const char* statusText(int s) {
    switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

// 请求级并发槽 RAII（构造占槽、析构释放；空闲 keep-alive 不持有）
struct ActiveSlot {
    ServerImpl* sv;
    bool held = false;
    // try-acquire：超限则不持有（计数立即回滚），调用方据 held 回 503
    ActiveSlot(ServerImpl* s) : sv(s) {
        int v = s->active.fetch_add(1) + 1;
        if (v > kMaxConcurrent) { s->active.fetch_sub(1); held = false; }
        else held = true;
    }
    ~ActiveSlot() { if (held) sv->active.fetch_sub(1); }
};

void handleConnection(ConnImpl& c) {
    ServerImpl* sv = c.server.get();
    // 请求循环（keep-alive）
    while (sv->running.load()) {
        std::string line;
        if (!readLineSock(c, line, 120000)) break; // 空闲 120s 断开
        if (line.empty()) { continue; }
        LOG_D("hsrv: request line=%s", line.c_str());
        HttpRequest req;
        size_t sp1 = line.find(' ');
        size_t sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            sendStatus(c, 400, "Bad Request");
            c.sendAll("Content-Length: 0\r\nConnection: close\r\n\r\n", 44);
            break;
        }
        req.method = line.substr(0, sp1);
        req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string ver = line.substr(sp2 + 1);
        size_t q = req.target.find('?');
        if (q == std::string::npos) {
            req.path = req.target;
        } else {
            req.path = req.target.substr(0, q);
            req.query = req.target.substr(q + 1);
        }
        // 头
        size_t contentLen = 0;
        bool expect100 = false;
        while (true) {
            std::string hl;
            if (!readLineSock(c, hl, 30000)) { c.m_goneFlag = true; break; }
            if (hl.empty()) break;
            size_t cp = hl.find(':');
            if (cp == std::string::npos) continue;
            std::string k = hl.substr(0, cp);
            std::string v = hl.substr(cp + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
            for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
            req.headers[k] = v;
            if (k == "content-length") contentLen = (size_t)_strtoui64(v.c_str(), nullptr, 10);
            if (k == "expect" && v.find("100-continue") != std::string::npos) expect100 = true;
            if (k == "connection" && v.find("close") != std::string::npos) c.keepAlive = false;
        }
        if (c.m_goneFlag) break;
        if (ver == "HTTP/1.0") c.keepAlive = false;
        LOG_D("hsrv: headers done clen=%d", (int)contentLen);
        if (contentLen > kMaxBody) {
            static const char kTooLarge[] =
                "Content-Length: 26\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nbody too large\r\n";
            sendStatus(c, 400, "Bad Request");
            c.sendAll(kTooLarge, strlen(kTooLarge));
            break;
        }
        if (expect100) c.sendAll("HTTP/1.1 100 Continue\r\n\r\n", 25);
        // body：先消费行缓冲中已到达的字节，再从 socket 读
        req.body.reserve(contentLen);
        while (req.body.size() < contentLen) {
            if (!c.lineBuf.empty()) {
                size_t take = std::min(c.lineBuf.size(), contentLen - req.body.size());
                req.body.append(c.lineBuf, 0, take);
                c.lineBuf.erase(0, take);
                continue;
            }
            char tmp[65536];
            int want = (int)std::min<size_t>(sizeof(tmp), contentLen - req.body.size());
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(c.s, &fds);
            timeval tv{ 300, 0 };
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) { c.m_goneFlag = true; break; }
            int n = ::recv(c.s, tmp, want, 0);
            if (n <= 0) { c.m_goneFlag = true; break; }
            req.body.append(tmp, n);
        }
        if (c.m_goneFlag) break;
        LOG_D("hsrv: body done len=%d", (int)req.body.size());

        // 请求级并发闸门：body 读完才占槽，keep-alive 空闲连接不占名额。
        // 超限时回 503 且不断连接，客户端可在同一连接重试。
        ActiveSlot guard(sv);
        if (!guard.held) {
            static const char kBusy[] =
                "Content-Type: application/json\r\nContent-Length: 23\r\n"
                "Connection: keep-alive\r\n\r\n{\"error\":\"server busy\"}";
            sendStatus(c, 503, "Service Unavailable");
            c.sendAll(kBusy, strlen(kBusy));
            if (c.m_goneFlag.load()) break;
            continue;
        }

        HttpResponseWriter w;
        w.impl = &c;
        bool handlerFatal = false;
        try {
            sv->handler(req, w);
        } catch (const std::exception& e) {
            LOG_E("请求处理异常 %s: %s", req.path.c_str(), e.what());
            handlerFatal = true;
        } catch (...) {
            LOG_E("请求处理未知异常 %s", req.path.c_str());
            handlerFatal = true;
        }
        if (handlerFatal) {
            if (!w.responded() && !c.m_goneFlag.load()) {
                static const char kErr[] =
                    "Content-Type: application/json\r\nContent-Length: 26\r\n"
                    "Connection: close\r\n\r\n{\"error\":\"internal error\"}";
                sendStatus(c, 500, "Internal Server Error");
                c.sendAll(kErr, strlen(kErr));
            }
            break; // 异常后连接内字节流状态不可信，直接关闭
        }
        if (!w.responded()) {
            static const char kNoResponse[] =
                "Content-Type: application/json\r\nContent-Length: 31\r\nConnection: close\r\n\r\n"
                "{\"error\":\"no handler response\"}\r\n";
            sendStatus(c, 500, "Internal Server Error");
            c.sendAll(kNoResponse, strlen(kNoResponse));
            break;
        }
        if (c.m_goneFlag.load() || !c.keepAlive) break;
    }
    // socket 由 ConnImpl 析构关闭
}

} // namespace

void HttpResponseWriter::respond(int status, const std::string& contentType, const std::string& body,
                                 const std::vector<std::pair<std::string, std::string>>& extra) {
    if (m_responded || !impl) return;
    auto* c = (ConnImpl*)impl;
    char head[512];
    std::string h;
    snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n", status, statusText(status));
    h = head;
    std::string ct = contentType;
    if (ct.rfind("application/json", 0) == 0 && ct.find("charset") == std::string::npos)
        ct += "; charset=utf-8";
    h += "Content-Type: " + ct + "\r\n";
    h += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    h += "Access-Control-Allow-Origin: *\r\n";
    for (auto& p : extra) h += p.first + ": " + p.second + "\r\n";
    h += "\r\n";
    if (!c->sendAll(h.data(), h.size())) { m_gone = true; return; }
    if (!body.empty() && !c->sendAll(body.data(), body.size())) { m_gone = true; return; }
    m_responded = true;
}

bool HttpResponseWriter::beginStream(int status, const std::string& contentType) {
    if (m_responded || !impl) return false;
    auto* c = (ConnImpl*)impl;
    char head[512];
    snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n", status, statusText(status));
    std::string h = head;
    h += "Content-Type: " + contentType + "\r\n";
    h += "Cache-Control: no-cache\r\n";
    h += "Access-Control-Allow-Origin: *\r\n";
    h += "Transfer-Encoding: chunked\r\n";
    h += "\r\n";
    if (!c->sendAll(h.data(), h.size())) { m_gone = true; return false; }
    c->chunked = true;
    m_responded = true;
    return true;
}

bool HttpResponseWriter::writeRaw(const char* data, size_t len) {
    if (!impl || m_gone) return false;
    auto* c = (ConnImpl*)impl;
    if (c->chunked) {
        char sz[32];
        snprintf(sz, sizeof(sz), "%zX\r\n", len);
        if (!c->sendAll(sz, strlen(sz))) { m_gone = true; return false; }
        if (len && !c->sendAll(data, len)) { m_gone = true; return false; }
        if (!c->sendAll("\r\n", 2)) { m_gone = true; return false; }
        return true;
    }
    return c->sendAll(data, len);
}

bool HttpResponseWriter::writeSse(const std::string& event, const std::string& data) {
    std::string frame;
    if (!event.empty()) frame += "event: " + event + "\n";
    frame += "data: " + data + "\n\n";
    return writeRaw(frame.data(), frame.size());
}

void HttpResponseWriter::endStream() {
    if (!impl) return;
    auto* c = (ConnImpl*)impl;
    if (c->chunked) {
        c->sendAll("0\r\n\r\n", 5);
        c->chunked = false;
    }
}

bool HttpServer::start(const std::string& host, int port, std::string& err) {
    static WsaInit wsa;
    if (m_impl) stop();
    auto impl = std::make_shared<ServerImpl>();
    impl->handler = handler;
    impl->listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl->listen == INVALID_SOCKET) {
        err = "socket 创建失败";
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(impl->listen, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (host.empty() || host == "0.0.0.0") addr.sin_addr.s_addr = INADDR_ANY;
    else inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (bind(impl->listen, (sockaddr*)&addr, sizeof(addr)) != 0) {
        err = "端口 " + std::to_string(port) + " 绑定失败（可能被占用）";
        closesocket(impl->listen);
        return false;
    }
    if (listen(impl->listen, 64) != 0) {
        err = "listen 失败";
        closesocket(impl->listen);
        return false;
    }
    impl->running = true;
    // m_impl 持有 shared_ptr（cpp 内部以 new shared_ptr 存放，头文件保持 void*）
    m_impl = new std::shared_ptr<ServerImpl>(impl);
    impl->acceptThread = std::thread([impl]() {
        while (impl->running.load()) {
            SOCKET s = accept(impl->listen, nullptr, nullptr);
            if (s == INVALID_SOCKET) {
                if (!impl->running.load()) break; // 停机关闭 listen 触发
                continue;
            }
            try {
                auto conn = std::make_shared<ConnImpl>(s, impl);
                impl->registerConn(conn);
                // 工作线程 detach 自回收：线程持有连接的 shared_ptr，
                // 结束时自动从注册表注销并关闭 socket，不累积线程对象。
                std::thread([conn]() {
                    try {
                        handleConnection(*conn);
                    } catch (const std::exception& e) {
                        LOG_E("连接线程异常: %s", e.what());
                    } catch (...) {
                        LOG_E("连接线程未知异常");
                    }
                    if (conn->server) conn->server->unregisterConn(conn);
                }).detach();
            } catch (const std::exception& e) {
                LOG_E("接受连接失败: %s", e.what());
                closesocket(s);
            } catch (...) {
                LOG_E("接受连接未知失败");
                closesocket(s);
            }
        }
    });
    LOG_I("HTTP 服务已启动 %s:%d", host.c_str(), port);
    return true;
}

void HttpServer::stop() {
    if (!m_impl) return;
    auto* p = (std::shared_ptr<ServerImpl>*)m_impl;
    std::shared_ptr<ServerImpl> impl = *p;
    delete p;
    m_impl = nullptr;

    impl->running = false;
    if (impl->listen != INVALID_SOCKET) {
        closesocket(impl->listen); // 打断 accept
        impl->listen = INVALID_SOCKET;
    }
    if (impl->acceptThread.joinable()) impl->acceptThread.join();
    // 打断所有在途连接的阻塞收发，使其工作线程尽快退出
    std::vector<std::shared_ptr<ConnImpl>> snapshot;
    {
        std::lock_guard<std::mutex> lk(impl->connMtx);
        snapshot = impl->conns;
        impl->conns.clear(); // 摘除服务器侧引用；socket 由工作线程的 ConnImpl 析构关闭
    }
    for (auto& c : snapshot) c->shutdown();
    // 不 join detach 的工作线程：它们各自持有 impl 的 shared_ptr，
    // 全部退出后 ServerImpl 自动释放；shutdown 已保证阻塞调用很快解除。
}
