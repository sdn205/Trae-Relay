// SseParser.cpp
#include "common/SseParser.h"
#include <cstring>

void SseParser::feed(const char* data, size_t len) {
    size_t start = 0;
    while (start < len) {
        const char* nl = (const char*)memchr(data + start, '\n', len - start);
        if (!nl) {
            m_lineBuf.append(data + start, len - start);
            return;
        }
        m_lineBuf.append(data + start, (size_t)(nl - (data + start)));
        start = (size_t)(nl - data) + 1;
        std::string line = std::move(m_lineBuf);
        m_lineBuf.clear();
        processLine(line);
    }
}

void SseParser::processLine(std::string& line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {
        // 空行：派发当前事件
        if (!m_data.empty() || !m_event.empty()) {
            std::string ev = m_event;
            std::string d = m_data;
            m_event.clear();
            m_data.clear();
            if (m_handler && !m_handler(ev, d)) m_handler = nullptr;
        }
        return;
    }
    if (line[0] == ':') return; // 注释/心跳
    if (line.rfind("event:", 0) == 0) {
        size_t s = 6;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        m_event = line.substr(s);
    } else if (line.rfind("data:", 0) == 0) {
        size_t s = 5;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        if (!m_data.empty()) m_data += '\n';
        m_data += line.substr(s);
    }
    // 其余字段（id:/retry:）忽略
}

void SseParser::finish() {
    std::string line = std::move(m_lineBuf);
    m_lineBuf.clear();
    if (!line.empty()) processLine(line);
    if (!m_data.empty()) {
        std::string ev = m_event;
        std::string d = m_data;
        m_event.clear();
        m_data.clear();
        if (m_handler && !m_handler(ev.empty() ? "message" : ev, d)) m_handler = nullptr;
    }
}
