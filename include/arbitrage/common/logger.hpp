#pragma once

#include <memory>
#include <string>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <sstream>
#include <map>
#include <mutex>

namespace arbitrage {

// 로그 레벨
enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5
};

// 간단한 로거 구현
class SimpleLogger {
public:
    SimpleLogger(const std::string& name) : name_(name) {}

    template<typename... Args>
    void trace(const std::string& fmt, Args&&... args) {
        log(LogLevel::Trace, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void debug(const std::string& fmt, Args&&... args) {
        log(LogLevel::Debug, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(const std::string& fmt, Args&&... args) {
        log(LogLevel::Info, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warn(const std::string& fmt, Args&&... args) {
        log(LogLevel::Warn, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(const std::string& fmt, Args&&... args) {
        log(LogLevel::Error, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void critical(const std::string& fmt, Args&&... args) {
        log(LogLevel::Critical, fmt, std::forward<Args>(args)...);
    }

    static LogLevel min_level_;
    static std::ofstream log_file_;
    static std::mutex file_mutex_;

private:
    std::string name_;

    template<typename... Args>
    void log(LogLevel level, const std::string& fmt, Args&&... args) {
        if (level < min_level_) return;

        // 시간 포맷
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);

        // 레벨 문자열
        const char* level_str = nullptr;
        switch (level) {
            case LogLevel::Trace:    level_str = "TRACE"; break;
            case LogLevel::Debug:    level_str = "DEBUG"; break;
            case LogLevel::Info:     level_str = "INFO "; break;
            case LogLevel::Warn:     level_str = "WARN "; break;
            case LogLevel::Error:    level_str = "ERROR"; break;
            case LogLevel::Critical: level_str = "CRIT "; break;
        }

        // 출력
        std::stringstream ss;
        ss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] "
           << "[" << level_str << "] "
           << "[" << name_ << "] ";

        // 간단한 포맷 문자열 처리
        format_string(ss, fmt, std::forward<Args>(args)...);

        std::string log_line = ss.str();

        // 콘솔 출력
        std::cerr << log_line << std::endl;

        // 파일 출력
        if (log_file_.is_open()) {
            std::lock_guard<std::mutex> lock(file_mutex_);
            log_file_ << log_line << std::endl;
        }
    }

    // 간단한 포맷 함수
    template<typename T, typename... Args>
    void format_string(std::stringstream& ss, const std::string& fmt, T&& first, Args&&... args) {
        auto pos = fmt.find("{}");
        if (pos != std::string::npos) {
            ss << fmt.substr(0, pos) << first;
            format_string(ss, fmt.substr(pos + 2), std::forward<Args>(args)...);
        } else {
            ss << fmt;
        }
    }

    void format_string(std::stringstream& ss, const std::string& fmt) {
        ss << fmt;
    }
};

class Logger {
public:
    // 초기화
    static void init(
        const std::string& log_dir = "logs",
        LogLevel console_level = LogLevel::Info,
        LogLevel file_level = LogLevel::Debug
    );

    // 로거 생성/조회
    static std::shared_ptr<SimpleLogger> create(const std::string& name);
    static std::shared_ptr<SimpleLogger> get(const std::string& name);

    // 기본 로거
    static std::shared_ptr<SimpleLogger> default_logger();

    // 종료
    static void shutdown();

private:
    static std::map<std::string, std::shared_ptr<SimpleLogger>> loggers_;
    static std::mutex mutex_;
    static bool initialized_;
};

}  // namespace arbitrage
