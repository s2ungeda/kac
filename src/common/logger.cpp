#include "arbitrage/common/logger.hpp"

namespace arbitrage {

// static 멤버 변수 정의
LogLevel SimpleLogger::min_level_ = LogLevel::Info;
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
    std::cerr << "[INFO] Logger initialized with level: " 
              << static_cast<int>(console_level) << std::endl;
    
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
}

}  // namespace arbitrage