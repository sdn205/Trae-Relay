// Log.h - 轻量日志：按日滚动文件 + 调试输出，线程安全
#pragma once
#include <string>

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

void logInit(const std::string& dir, LogLevel level, int retainDays, bool enabled = true);
void logSetEnabled(bool enabled);
void logSetLevel(LogLevel level);
LogLevel logLevel();
bool logEnabled(LogLevel lv);
void logWrite(LogLevel lv, const char* fmt, ...);

// 敏感值脱敏：保留前 6 后 4，中间 *
std::string logRedact(const std::string& secret);

#define LOG_T(...) do { if (logEnabled(LogLevel::Trace)) logWrite(LogLevel::Trace, __VA_ARGS__); } while (0)
#define LOG_D(...) do { if (logEnabled(LogLevel::Debug)) logWrite(LogLevel::Debug, __VA_ARGS__); } while (0)
#define LOG_I(...) do { if (logEnabled(LogLevel::Info)) logWrite(LogLevel::Info, __VA_ARGS__); } while (0)
#define LOG_W(...) do { if (logEnabled(LogLevel::Warn)) logWrite(LogLevel::Warn, __VA_ARGS__); } while (0)
#define LOG_E(...) logWrite(LogLevel::Error, __VA_ARGS__)
