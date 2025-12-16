#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/common/config.hpp"
#include "arbitrage/exchange/upbit/websocket.hpp"
#include "arbitrage/exchange/binance/websocket.hpp"
#include "arbitrage/exchange/bithumb/websocket.hpp"
#include "arbitrage/exchange/mexc/websocket.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <atomic>

using namespace arbitrage;

// 매트릭스를 예쁘게 출력
void print_matrix(const PremiumMatrix& matrix) {
    const char* exchanges[] = {"Upbit", "Bithumb", "Binance", "MEXC"};
    
    std::cout << "\n=== Premium Matrix (%) ===\n\n";
    std::cout << "       Buy →\n";
    std::cout << "Sell ↓ ";
    
    // 헤더
    for (int i = 0; i < 4; ++i) {
        std::cout << std::setw(9) << exchanges[i];
    }
    std::cout << "\n";
    
    // 매트릭스
    for (int sell = 0; sell < 4; ++sell) {
        std::cout << std::setw(7) << exchanges[sell];
        for (int buy = 0; buy < 4; ++buy) {
            double premium = matrix[buy][sell];
            std::cout << std::setw(9) << std::fixed << std::setprecision(2);
            
            if (std::isnan(premium)) {
                std::cout << "N/A";
            } else if (premium > 3.0) {
                // 3% 이상은 강조
                std::cout << "*" << premium << "*";
            } else {
                std::cout << premium;
            }
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

int main() {
    // Logger 초기화
    Logger::init("premium_example");
    
    std::cout << "=== Premium Calculator Example (with Real Data) ===\n\n";
    
    // Config 로드
    if (!Config::instance().load("config/config.yaml")) {
        std::cerr << "Failed to load config" << std::endl;
        return 1;
    }
    
    // IO Context와 SSL Context 생성
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tlsv12_client};
    ssl_ctx.set_default_verify_paths();
    
    // WebSocket 클라이언트 생성
    auto upbit_ws = std::make_shared<UpbitWebSocket>(ioc, ssl_ctx);
    auto binance_ws = std::make_shared<BinanceWebSocket>(ioc, ssl_ctx);
    auto bithumb_ws = std::make_shared<BithumbWebSocket>(ioc, ssl_ctx);
    auto mexc_ws = std::make_shared<MEXCWebSocket>(ioc, ssl_ctx);
    
    // FXRate 서비스
    FXRateService fx_service;
    
    PremiumCalculator calculator;
    
    // 1. 임계값 및 콜백 설정
    calculator.set_threshold(2.5);  // 2.5% 이상만 알림
    
    int alert_count = 0;
    calculator.on_premium_changed([&alert_count](const PremiumInfo& info) {
        alert_count++;
        std::cout << "\n🔔 ALERT #" << alert_count << ": Premium opportunity detected!\n";
        std::cout << "   Route: " << exchange_name(info.buy_exchange) 
                  << " → " << exchange_name(info.sell_exchange) << "\n";
        std::cout << "   Premium: " << std::fixed << std::setprecision(2) 
                  << info.premium_pct << "%\n";
        std::cout << "   Buy at: " << info.buy_price << " KRW\n";
        std::cout << "   Sell at: " << info.sell_price << " KRW\n";
        std::cout << "   FX Rate: " << info.fx_rate << " KRW/USD\n";
    });
    
    // 2. 실제 환율 가져오기
    std::cout << "Fetching real FX rate...\n";
    auto fx_result = fx_service.fetch();
    if (fx_result) {
        calculator.update_fx_rate(fx_result.value().rate);
        std::cout << "FX Rate: " << fx_result.value().rate << " KRW/USD\n\n";
    } else {
        std::cout << "Failed to fetch FX rate, using default 1320.0\n";
        calculator.update_fx_rate(1320.0);
    }
    
    // 실시간 가격 업데이트를 위한 원자 변수
    std::atomic<double> last_upbit_price{0};
    std::atomic<double> last_binance_price{0};
    std::atomic<double> last_bithumb_price{0};
    std::atomic<double> last_mexc_price{0};
    
    // WebSocket 이벤트 핸들러
    upbit_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker()) {
            double price = evt.ticker().price;
            last_upbit_price = price;
            calculator.update_price(Exchange::Upbit, price);
        }
    });
    
    binance_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker()) {
            double price = evt.ticker().price;
            last_binance_price = price;
            calculator.update_price(Exchange::Binance, price);
        }
    });
    
    bithumb_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker()) {
            double price = evt.ticker().price;
            last_bithumb_price = price;
            calculator.update_price(Exchange::Bithumb, price);
        }
    });
    
    mexc_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker()) {
            double price = evt.ticker().price;
            last_mexc_price = price;
            calculator.update_price(Exchange::MEXC, price);
        }
    });
    
    // Config에서 첫 번째 primary 심볼 사용 (예: XRP)
    const auto& primary_symbols = Config::instance().primary_symbols();
    if (primary_symbols.empty()) {
        std::cerr << "No primary symbols configured" << std::endl;
        return 1;
    }
    
    const auto& first_symbol = primary_symbols[0];
    std::cout << "Monitoring symbol: " << first_symbol.symbol << "\n\n";
    
    // 각 거래소별 심볼 구독
    upbit_ws->subscribe_ticker({first_symbol.upbit});
    binance_ws->subscribe_ticker({first_symbol.binance});
    bithumb_ws->subscribe_ticker({first_symbol.bithumb});
    mexc_ws->subscribe_ticker({first_symbol.mexc});
    
    // WebSocket 연결 시작
    std::cout << "Connecting to exchanges...\n";
    upbit_ws->connect("api.upbit.com", "443", "/websocket/v1");
    binance_ws->connect("stream.binance.com", "9443", "/stream?streams=xrpusdt@ticker");
    bithumb_ws->connect("pubwss.bithumb.com", "443", "/pub/ws");
    mexc_ws->connect("wbs-api.mexc.com", "443", "/ws");
    
    // IO 스레드 시작
    std::thread io_thread([&ioc]() {
        ioc.run();
    });
    
    // 3. 현재 매트릭스 출력
    auto matrix = calculator.get_matrix();
    print_matrix(matrix);
    
    // 4. 최고 기회 조회
    auto best = calculator.get_best_opportunity();
    if (best) {
        std::cout << "📈 Best opportunity:\n";
        std::cout << "   " << exchange_name(best->buy_exchange) 
                  << " → " << exchange_name(best->sell_exchange)
                  << ": " << best->premium_pct << "%\n\n";
    }
    
    // 5. 2% 이상 기회들 조회
    std::cout << "Opportunities > 2%:\n";
    auto opportunities = calculator.get_opportunities(2.0);
    for (const auto& opp : opportunities) {
        std::cout << "   " << exchange_name(opp.buy_exchange) 
                  << " → " << exchange_name(opp.sell_exchange)
                  << ": " << std::setw(5) << std::fixed << std::setprecision(2) 
                  << opp.premium_pct << "%\n";
    }
    
    // 6. 실시간 데이터 모니터링
    std::cout << "\n\n=== Starting real-time monitoring (30 seconds) ===\n";
    std::cout << "Waiting for price updates from exchanges...\n\n";
    
    // 실시간 가격 표시 및 매트릭스 업데이트
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // 현재 가격 표시 (10초마다)
        if (i % 10 == 0) {
            std::cout << "\nCurrent Prices:\n";
            if (last_upbit_price > 0) 
                std::cout << "  Upbit: " << last_upbit_price.load() << " KRW\n";
            if (last_binance_price > 0)
                std::cout << "  Binance: " << last_binance_price.load() << " USDT\n";
            if (last_bithumb_price > 0)
                std::cout << "  Bithumb: " << last_bithumb_price.load() << " KRW\n";
            if (last_mexc_price > 0)
                std::cout << "  MEXC: " << last_mexc_price.load() << " USDT\n";
            
            print_matrix(calculator.get_matrix());
        }
    }
    
    // IO 스레드 정리
    ioc.stop();
    io_thread.join();
    
    // 7. 최종 통계
    std::cout << "\n=== Final Statistics ===\n";
    std::cout << "Total alerts triggered: " << alert_count << "\n";
    
    auto final_best = calculator.get_best_opportunity();
    if (final_best) {
        std::cout << "Final best premium: " << final_best->premium_pct << "%\n";
    }
    
    return 0;
}