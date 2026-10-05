#include "common/Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace aes67 {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
LogLevel g_min = LogLevel::Info;

LogLevel ParseLevel(const char* s) {
  if (!s) return LogLevel::Info;
  if (!_stricmp(s, "trace")) return LogLevel::Trace;
  if (!_stricmp(s, "debug")) return LogLevel::Debug;
  if (!_stricmp(s, "info")) return LogLevel::Info;
  if (!_stricmp(s, "warn")) return LogLevel::Warn;
  if (!_stricmp(s, "error")) return LogLevel::Error;
  return LogLevel::Info;
}

const char* LevelTag(LogLevel l) {
  switch (l) {
    case LogLevel::Trace: return "TRC";
    case LogLevel::Debug: return "DBG";
    case LogLevel::Info: return "INF";
    case LogLevel::Warn: return "WRN";
    case LogLevel::Error: return "ERR";
  }
  return "???";
}

}

void LogInit(const char* severity) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_min = ParseLevel(severity);
  if (!g_file) {
    const char* pd = std::getenv("ProgramData");
    std::string dir = (pd ? pd : "C:\\ProgramData");
    dir += "\\AES67Bridge\\logs";
    CreateDirectoryA((std::string(pd ? pd : "C:\\ProgramData") + "\\AES67Bridge").c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string path = dir + "\\aes67bridge.log";
    g_file = fopen(path.c_str(), "a");
  }
}

void LogLine(LogLevel level, const char* fmt, ...) {
  if (static_cast<int>(level) < static_cast<int>(g_min)) return;

  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  SYSTEMTIME st;
  GetLocalTime(&st);
  char line[2200];
  snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n", st.wHour,
           st.wMinute, st.wSecond, st.wMilliseconds, LevelTag(level), msg);

  std::lock_guard<std::mutex> lk(g_mutex);
  OutputDebugStringA(line);
  if (g_file) {
    fputs(line, g_file);
    fflush(g_file);
  }
}

void LogShutdown() {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_file) {
    fclose(g_file);
    g_file = nullptr;
  }
}

}
