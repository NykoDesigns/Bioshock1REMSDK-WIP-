#include "log.h"

#include <Windows.h>
#include <chrono>
#include <iostream>

namespace bs1sdk {

std::ofstream Log::s_File;
LogLevel Log::s_MinLevel = LogLevel::Info;
std::mutex Log::s_Mutex;
bool Log::s_Initialized = false;

static const char* LevelToString(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Fatal: return "FATAL";
    default: return "?????";
    }
}

void Log::Initialize(const std::string& filename, LogLevel minLevel)
{
    std::lock_guard lock(s_Mutex);
    if (s_Initialized) return;

    s_MinLevel = minLevel;
    s_File.open(filename, std::ios::out | std::ios::trunc);
    s_Initialized = true;

    // Allocate a console for debug output
    AllocConsole();
    SetConsoleTitleA("BS1SDK Debug Console");
    
    // Redirect stdout to console
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
}

void Log::Shutdown()
{
    std::lock_guard lock(s_Mutex);
    if (!s_Initialized) return;

    s_File.close();
    FreeConsole();
    s_Initialized = false;
}

void Log::SetLevel(LogLevel level)
{
    s_MinLevel = level;
}

void Log::Write(LogLevel level, const std::string& message)
{
    if (level < s_MinLevel) return;
    if (!s_Initialized) return;

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_buf;
    localtime_s(&tm_buf, &time);

    char timeBuf[32];
    std::snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d.%03d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, 
                  static_cast<int>(ms.count()));

    std::string formatted = std::format("[{}] [{}] {}\n", timeBuf, LevelToString(level), message);

    std::lock_guard lock(s_Mutex);
    
    // Write to file
    if (s_File.is_open()) {
        s_File << formatted;
        s_File.flush();
    }

    // Write to console
    std::cout << formatted;

    // OutputDebugString for debugger
    OutputDebugStringA(formatted.c_str());
}

} // namespace bs1sdk
