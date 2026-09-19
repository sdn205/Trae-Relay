// HttpServer.h - 回环 HTTP/1.1 服务（keep-alive + chunked SSE）
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

struct HttpRequest {
    std::string method, target, path, query, body;
    std::map<std::string, std::string> headers; // 键小写
    const std::string* header(const std::string& k) const {
        auto it = headers.find(k);
        return it == headers.end() ? nullptr : &it->second;
    }
};

class HttpResponseWriter {
public:
    void respond(int status, const std::string& contentType, const std::string& body,
                 const std::vector<std::pair<std::string, std::string>>& extra = {});
    bool beginStream(int status, const std::string& contentType);
    bool writeRaw(const char* data, size_t len);
    bool writeSse(const std::string& event, const std::string& data); // event 可为空
    void endStream();
    bool responded() const { return m_responded; }
    bool clientGone() const { return m_gone; }

    // 内部（由 HttpServer 设置）
    void* impl = nullptr;
    bool m_responded = false;
    bool m_gone = false;
};

using HttpHandler = std::function<void(const HttpRequest&, HttpResponseWriter&)>;

class HttpServer {
public:
    HttpHandler handler;
    bool start(const std::string& host, int port, std::string& err);
    void stop();
    ~HttpServer() { stop(); }

private:
    void* m_impl = nullptr;
};
