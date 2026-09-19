// SseParser.h - 增量 SSE 解析（无回溯、低分配）
#pragma once
#include <functional>
#include <string>

// 解析 text/event-stream：event:/data: 行，空行派发，':' 注释忽略
class SseParser {
public:
    // 返回 true 表示消费继续；false 表示调用方要求中止
    void setHandler(std::function<bool(const std::string& event, const std::string& data)> h) {
        m_handler = std::move(h);
    }
    // 喂入原始字节；内部凑行后回调
    void feed(const char* data, size_t len);
    // 流结束时调用：若还有未派发的缓冲（无空行结尾），派发一次
    void finish();

private:
    void processLine(std::string& line);
    std::string m_lineBuf;
    std::string m_event;
    std::string m_data;
    std::function<bool(const std::string&, const std::string&)> m_handler;
};
