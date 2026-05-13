#pragma once

#include <string>
#include <format>
#include <fstream>
#include <mutex>
#include <cstdint>

namespace bs1sdk {

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

/// Lightweight logging system. Writes to file and optional console.
/// Uses std::format for type-safe formatting.
class Log {
public:
    static void Initialize(const std::string& filename = "bs1sdk.log", LogLevel minLevel = LogLevel::Info);
    static void Shutdown();

    static void SetLevel(LogLevel level);
    static void Write(LogLevel level, const std::string& message);

    template<typename... Args>
    static void Trace(std::format_string<Args...> fmt, Args&&... args) {
        Write(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void Debug(std::format_string<Args...> fmt, Args&&... args) {
        Write(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void Info(std::format_string<Args...> fmt, Args&&... args) {
        Write(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void Warn(std::format_string<Args...> fmt, Args&&... args) {
        Write(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void Error(std::format_string<Args...> fmt, Args&&... args) {
        Write(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void Fatal(std::format_string<Args...> fmt, Args&&... args) {
        Write(LogLevel::Fatal, std::format(fmt, std::forward<Args>(args)...));
    }

private:
    static std::ofstream s_File;
    static LogLevel s_MinLevel;
    static std::mutex s_Mutex;
    static bool s_Initialized;
};

// Convenience macros that include source location
#define LOG_TRACE(...) ::bs1sdk::Log::Trace(__VA_ARGS__)
#define LOG_DEBUG(...) ::bs1sdk::Log::Debug(__VA_ARGS__)
#define LOG_INFO(...)  ::bs1sdk::Log::Info(__VA_ARGS__)
#define LOG_WARN(...)  ::bs1sdk::Log::Warn(__VA_ARGS__)
#define LOG_ERROR(...) ::bs1sdk::Log::Error(__VA_ARGS__)
#define LOG_FATAL(...) ::bs1sdk::Log::Fatal(__VA_ARGS__)

} // namespace bs1sdk
