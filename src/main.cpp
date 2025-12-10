#include "arbitrage/common/config.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/exchange/interface.hpp"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>

namespace {
    std::atomic<bool> g_running{true};
    
    void signal_handler(int signum) {
        arbitrage::Logger::default_logger()->info("Received signal {}, shutting down...", signum);
        g_running = false;
    }
}

int main(int argc, char* argv[]) {
    // 설정 파일 경로
    std::string config_path = "config/config.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }
    
    // 로거 초기화
    arbitrage::Logger::init("logs");
    auto logger = arbitrage::Logger::create("main");
    
    logger->info("Starting Kimchi Arbitrage System (C++)");
    logger->info("Config: {}", config_path);
    
    // 설정 로드
    if (!arbitrage::Config::instance().load(config_path)) {
        logger->error("Failed to load config");
        return 1;
    }
    
    // 시그널 핸들러
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    logger->info("System initialized successfully");
    
    // 메인 루프 (Busy wait)
    while (g_running) {
        // TODO: 여기에 실제 로직 추가
        // - 이벤트 큐 처리
        // - 아비트라지 기회 계산
        // - 주문 실행 등
        
        // CPU에 힌트 제공 (spin-wait 최적화)
        __builtin_ia32_pause();
    }
    
    // 정리
    logger->info("Shutting down...");
    arbitrage::Logger::shutdown();
    
    return 0;
}