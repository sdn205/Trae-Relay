// Log.cpp
#include "common/Log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace {
struct LogState {
    std::mutex mtx;
    std::string dir;
    LogLevel level = LogLevel::Info;
    int retainDays = 7;
    std::string curDate;
    FILE* fp = nullptr;
};
LogState g;

// 日志目录可能位于含中文用户名的路径下，文件 IO 一律走宽字符接口。
std::wstring widenLog(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
const char* levelName(LogLevel lv) {
    switch (lv) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warn: return "WARN";
    default: return "ERROR";
    }
}
std::string todayStr() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
    return buf;
}
void openFileLocked() {
    if (g.fp) { fclose(g.fp); g.fp = nullptr; }
    std::string date = todayStr();
    if (!g.dir.empty()) {
        std::wstring path = widenLog(g.dir + "\\traerelay-" + date + ".log");
        g.fp = _wfsopen(path.c_str(), L"a", _SH_DENYWR); // UTF-8 追加
        if (g.fp) {
            fseek(g.fp, 0, SEEK_END);
            if (ftell(g.fp) == 0) { // 新文件写 BOM
                const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
                fwrite(bom, 1, 3, g.fp);
                fflush(g.fp);
            }
        }
    }
    g.curDate = date;
}
void pruneOldLocked() {
    if (g.dir.empty() || g.retainDays <= 0) return;
    WIN32_FIND_DATAW fd;
    std::wstring pat = widenLog(g.dir + "\\traerelay-*.log");
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now{ nowFt.dwLowDateTime, nowFt.dwHighDateTime };
    do {
        ULARGE_INTEGER t{ fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime };
        ULONGLONG diffDays = (now.QuadPart - t.QuadPart) / (10ULL * 1000 * 1000 * 60 * 60 * 24);
        if (diffDays > (ULONGLONG)g.retainDays) {
            std::wstring full = widenLog(g.dir) + L"\\" + fd.cFileName;
            DeleteFileW(full.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
} // namespace

void logInit(const std::string& dir, LogLevel level, int retainDays) {
    std::lock_guard<std::mutex> lk(g.mtx);
    g.dir = dir;
    g.level = level;
    g.retainDays = retainDays;
    if (!dir.empty()) {
        // 多级目录创建（宽字符，兼容中文路径）
        std::string cur;
        for (size_t i = 0; i < dir.size(); ++i) {
            char c = dir[i];
            cur += c;
            if (c == '\\' || c == '/' || i + 1 == dir.size()) {
                // 仅盘根（形如 E:\）跳过；注意不能用 >= 判断，否则 cur[2]
                // 对所有绝对路径前缀恒为反斜杠，会跳过每一级目录的创建。
                if (cur.size() == 3 && cur[1] == ':' && cur[2] == '\\') continue;
                CreateDirectoryW(widenLog(cur).c_str(), nullptr);
            }
        }
    }
    openFileLocked();
    pruneOldLocked();
}
void logSetLevel(LogLevel lv) { std::lock_guard<std::mutex> lk(g.mtx); g.level = lv; }
LogLevel logLevel() { return g.level; }
bool logEnabled(LogLevel lv) { return lv >= g.level; }

void logWrite(LogLevel lv, const char* fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[64];
    snprintf(head, sizeof(head), "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             levelName(lv));
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);
    size_t n = strlen(body);
    if (n && body[n - 1] != '\n') strcat_s(body, "\n");

    std::lock_guard<std::mutex> lk(g.mtx);
    if (g.fp) {
        std::string date = todayStr();
        if (date != g.curDate) openFileLocked();
    }
    OutputDebugStringA(head);
    OutputDebugStringA(body);
    if (g.fp) {
        fputs(head, g.fp);
        fputs(body, g.fp);
        fflush(g.fp);
    }
}

std::string logRedact(const std::string& secret) {
    if (secret.size() <= 10) return std::string(secret.size(), '*');
    return secret.substr(0, 6) + std::string(secret.size() - 10, '*') + secret.substr(secret.size() - 4);
}
