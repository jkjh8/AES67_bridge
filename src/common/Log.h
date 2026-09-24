#pragma once

namespace aes67 {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

void LogInit(const char* severity);
void LogLine(LogLevel level, const char* fmt, ...);
void LogShutdown();

}

#define LOGT(...) ::aes67::LogLine(::aes67::LogLevel::Trace, __VA_ARGS__)
#define LOGD(...) ::aes67::LogLine(::aes67::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::aes67::LogLine(::aes67::LogLevel::Info, __VA_ARGS__)
#define LOGW(...) ::aes67::LogLine(::aes67::LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) ::aes67::LogLine(::aes67::LogLevel::Error, __VA_ARGS__)
