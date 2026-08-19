#pragma once

#include <string>
#include <string_view>
#include <mutex>
#include <format>

namespace ifs {

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Critical
};

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel getLevel() const;

    void log(LogLevel level, std::string_view component, std::string_view message);

    template <typename... Args>
    void trace(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
        if (minLevel_ <= LogLevel::Trace)
            log(LogLevel::Trace, component, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void debug(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
        if (minLevel_ <= LogLevel::Debug)
            log(LogLevel::Debug, component, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void info(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
        if (minLevel_ <= LogLevel::Info)
            log(LogLevel::Info, component, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warn(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
        if (minLevel_ <= LogLevel::Warn)
            log(LogLevel::Warn, component, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void error(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
        if (minLevel_ <= LogLevel::Error)
            log(LogLevel::Error, component, std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void critical(std::string_view component, std::format_string<Args...> fmt, Args&&... args) {
        if (minLevel_ <= LogLevel::Critical)
            log(LogLevel::Critical, component, std::format(fmt, std::forward<Args>(args)...));
    }

    static LogLevel parseLevel(std::string_view str);

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex mutex_;
    LogLevel minLevel_ = LogLevel::Info;
};

} // namespace ifs
