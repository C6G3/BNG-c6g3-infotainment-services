#include "core/Logger.hpp"

#include <iostream>
#include <chrono>
#include <ctime>
#include <algorithm>

namespace ifs {

namespace {

std::string_view levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return "TRACE";
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO ";
        case LogLevel::Warn:     return "WARN ";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRIT ";
        default:                 return "?????";
    }
}

} // anonymous namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard lock(mutex_);
    minLevel_ = level;
}

LogLevel Logger::getLevel() const {
    return minLevel_;
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message) {
    if (level < minLevel_) return;

    auto now      = std::chrono::system_clock::now();
    auto timeT    = std::chrono::system_clock::to_time_t(now);
    auto ms       = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;

    // Lock protects both std::localtime (static buffer) and console output
    std::lock_guard lock(mutex_);

    std::tm* tm = std::localtime(&timeT);

    std::string line = std::format(
        "[{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}] [{}] [{}] {}",
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
        tm->tm_hour, tm->tm_min, tm->tm_sec,
        static_cast<int>(ms.count()),
        levelToString(level), component, message);

    if (level >= LogLevel::Error) {
        std::cerr << line << '\n';
    } else {
        std::cout << line << '\n';
    }
}

LogLevel Logger::parseLevel(std::string_view str) {
    std::string lower(str);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "trace")    return LogLevel::Trace;
    if (lower == "debug")    return LogLevel::Debug;
    if (lower == "info")     return LogLevel::Info;
    if (lower == "warn")     return LogLevel::Warn;
    if (lower == "warning")  return LogLevel::Warn;
    if (lower == "error")    return LogLevel::Error;
    if (lower == "critical") return LogLevel::Critical;
    if (lower == "crit")     return LogLevel::Critical;

    return LogLevel::Info;
}

} // namespace ifs
