#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/common/logger.hpp"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>

using namespace arbitrage;

int main() {
    // Logger 초기화
    Logger::init("fxrate_example");
    
    std::cout << "=== FXRate Service Example ===\n\n";
    
    // FXRate 서비스 초기화
    FXRateService::global_init();
    FXRateService fxrate_service;
    
    // 1. 수동 환율 조회
    std::cout << "1. Manual fetch:\n";
    auto result = fxrate_service.fetch();
    
    if (result) {
        const auto& rate = result.value();
        std::cout << "   USD/KRW: " << std::fixed << std::setprecision(2) 
                  << rate.rate << "\n";
        std::cout << "   Source: " << rate.source << "\n";
        std::cout << "   Valid: " << (rate.is_valid() ? "Yes" : "No") << "\n\n";
    } else {
        std::cout << "   Failed: " << result.error().message << "\n\n";
    }
    
    // 2. 캐시된 환율 확인
    std::cout << "2. Cached rate:\n";
    auto cached = fxrate_service.get_cached();
    std::cout << "   USD/KRW: " << cached.rate << "\n";
    std::cout << "   Source: " << cached.source << "\n\n";
    
    // 3. 자동 갱신 with 콜백
    std::cout << "3. Auto refresh (every 10 seconds):\n";
    
    int update_count = 0;
    fxrate_service.on_rate_changed([&update_count](const FXRate& rate) {
        update_count++;
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        std::cout << "   [" << std::put_time(std::localtime(&time), "%H:%M:%S") << "] "
                  << "Update #" << update_count << ": "
                  << rate.rate << " KRW/USD (from " << rate.source << ")\n";
    });
    
    // 10초 간격으로 자동 갱신
    fxrate_service.start_auto_refresh(std::chrono::seconds(10));
    
    // 4. 김치 프리미엄 계산 예시
    std::cout << "\n4. Kimchi Premium Calculation:\n";
    
    double upbit_xrp_krw = 1850.0;     // Upbit XRP/KRW
    double binance_xrp_usdt = 1.35;    // Binance XRP/USDT
    
    auto current_rate = fxrate_service.get_cached();
    if (current_rate.is_valid()) {
        double binance_in_krw = binance_xrp_usdt * current_rate.rate;
        double premium = ((upbit_xrp_krw / binance_in_krw) - 1.0) * 100.0;
        
        std::cout << "   Upbit XRP: " << upbit_xrp_krw << " KRW\n";
        std::cout << "   Binance XRP: " << binance_xrp_usdt << " USDT\n";
        std::cout << "   USD/KRW Rate: " << current_rate.rate << "\n";
        std::cout << "   Binance in KRW: " << binance_in_krw << " KRW\n";
        std::cout << "   Kimchi Premium: " << std::fixed << std::setprecision(2) 
                  << premium << "%\n";
    }
    
    // 30초 동안 실행
    std::cout << "\n(Running for 30 seconds...)\n";
    std::this_thread::sleep_for(std::chrono::seconds(30));
    
    // 정리
    fxrate_service.stop_auto_refresh();
    FXRateService::global_cleanup();
    
    std::cout << "\nTotal updates received: " << update_count << "\n";
    std::cout << "Done!\n";
    
    return 0;
}