#include "arbitrage/common/logger.hpp"
#include <filesystem>
#include <ctime>

namespace arbitrage {

// static 멤버 변수 정의
LogLevel SimpleLogger::min_level_ = LogLevel::Info;
std::ofstream SimpleLogger::log_file_;
std::mutex SimpleLogger::file_mutex_;
std::map<std::string, std::shared_ptr<SimpleLogger>> Logger::loggers_;
std::mutex Logger::mutex_;
bool Logger::initialized_ = false;

void Logger::init(
    const std::string& log_dir,
    LogLevel console_level,
    LogLevel file_level
) {
    if (initialized_) return;

    SimpleLogger::min_level_ = console_level;

    // 로그 디렉토리 생성
    std::filesystem::create_directories(log_dir);

    // 날짜별 로그 파일 생성
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&time_t);

    char filename[256];
    std::snprintf(filename, sizeof(filename), "%s/arbitrage_%04d-%02d-%02d.log",
                  log_dir.c_str(),
                  tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

    SimpleLogger::log_file_.open(filename, std::ios::app);

    if (SimpleLogger::log_file_.is_open()) {
        std::cerr << "[INFO] Logger initialized - console level: "
                  << static_cast<int>(console_level)
                  << ", log file: " << filename << std::endl;
    } else {
        std::cerr << "[WARN] Failed to open log file: " << filename << std::endl;
    }

    initialized_ = true;
}

std::shared_ptr<SimpleLogger> Logger::create(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = loggers_.find(name);
    if (it != loggers_.end()) {
        return it->second;
    }

    auto logger = std::make_shared<SimpleLogger>(name);
    loggers_[name] = logger;
    return logger;
}

std::shared_ptr<SimpleLogger> Logger::get(const std::string& name) {
    return create(name);
}

std::shared_ptr<SimpleLogger> Logger::default_logger() {
    return create("default");
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    loggers_.clear();

    if (SimpleLogger::log_file_.is_open()) {
        SimpleLogger::log_file_.close();
    }
}

}  // namespace arbitrage
