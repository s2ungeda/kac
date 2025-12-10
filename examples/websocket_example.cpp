#include <iostream>
#include <chrono>
#include <thread>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include "arbitrage/exchange/upbit/websocket.hpp"
#include "arbitrage/exchange/binance/websocket.hpp"
#include "arbitrage/exchange/bithumb/websocket.hpp"
#include "arbitrage/exchange/mexc/websocket.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/config.hpp"

using namespace arbitrage;

int main() {
    try {
        // Logger 초기화 (디버그 레벨로 설정)
        Logger::init("logs", LogLevel::Debug, LogLevel::Debug);
        
        // IO Context와 SSL Context 생성
        boost::asio::io_context ioc;
        boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tlsv12_client};
        ssl_ctx.set_default_verify_paths();
        
        // WebSocket 클라이언트 생성
        auto upbit = std::make_shared<UpbitWebSocket>(ioc, ssl_ctx);
        auto binance = std::make_shared<BinanceWebSocket>(ioc, ssl_ctx);
        auto bithumb = std::make_shared<BithumbWebSocket>(ioc, ssl_ctx);
        auto mexc = std::make_shared<MEXCWebSocket>(ioc, ssl_ctx);
        
        // 이벤트 핸들러 설정
        auto event_handler = [](const WebSocketEvent& evt) {
            switch (evt.type) {
                case WebSocketEvent::Type::Connected:
                    std::cout << "[" << exchange_name(evt.exchange) 
                              << "] Connected\n";
                    break;
                    
                case WebSocketEvent::Type::Ticker:
                    std::cout << "[" << exchange_name(evt.exchange) 
                              << "] Ticker: " 
                              << evt.ticker().symbol << " @ " 
                              << evt.ticker().price << "\n";
                    break;
                    
                case WebSocketEvent::Type::OrderBook:
                    std::cout << "[" << exchange_name(evt.exchange) 
                              << "] OrderBook: " 
                              << evt.orderbook().symbol 
                              << " Best Bid: " << evt.orderbook().best_bid()
                              << " Best Ask: " << evt.orderbook().best_ask()
                              << "\n";
                    break;
                    
                case WebSocketEvent::Type::Error:
                    std::cout << "[" << exchange_name(evt.exchange) 
                              << "] Error: " << evt.error_message << "\n";
                    break;
                    
                default:
                    break;
            }
        };
        
        // 이벤트 핸들러 등록
        upbit->on_event(event_handler);
        binance->on_event(event_handler);
        bithumb->on_event(event_handler);
        mexc->on_event(event_handler);
        
        // Config에서 심볼 목록 가져오기
        if (!Config::instance().load("config/config.yaml")) {
            std::cerr << "Failed to load config" << std::endl;
            return 1;
        }
        
        // 각 거래소별 심볼 목록
        auto upbit_symbols = Config::instance().get_symbols_for_exchange(Exchange::Upbit);
        auto binance_symbols = Config::instance().get_symbols_for_exchange(Exchange::Binance);
        auto bithumb_symbols = Config::instance().get_symbols_for_exchange(Exchange::Bithumb);
        auto mexc_symbols = Config::instance().get_symbols_for_exchange(Exchange::MEXC);
        
        std::cout << "Loaded symbols from config:\n";
        std::cout << "  Upbit: ";
        for (const auto& s : upbit_symbols) std::cout << s << " ";
        std::cout << "\n  Binance: ";
        for (const auto& s : binance_symbols) std::cout << s << " ";
        std::cout << "\n  Bithumb: ";
        for (const auto& s : bithumb_symbols) std::cout << s << " ";
        std::cout << "\n  MEXC: ";
        for (const auto& s : mexc_symbols) std::cout << s << " ";
        std::cout << "\n\n";
        
        // 구독 설정
        upbit->subscribe_ticker(upbit_symbols);
        upbit->subscribe_orderbook(upbit_symbols);
        
        binance->subscribe_ticker(binance_symbols);
        binance->subscribe_orderbook(binance_symbols, 10);
        
        bithumb->subscribe_ticker(bithumb_symbols);
        bithumb->subscribe_orderbook(bithumb_symbols);
        
        mexc->subscribe_ticker(mexc_symbols);
        mexc->subscribe_orderbook(mexc_symbols);
        
        // 연결
        std::cout << "Connecting to exchanges...\n";
        
        upbit->connect("api.upbit.com", "443", "/websocket/v1");
        binance->connect_with_streams();  // Combined Stream 사용
        bithumb->connect("pubwss.bithumb.com", "443", "/pub/ws");
        mexc->connect("wbs.mexc.com", "443", "/ws");
        
        // IO 스레드 시작
        std::thread io_thread([&ioc]() {
            ioc.run();
        });
        
        // 메인 스레드에서 이벤트 큐 폴링
        std::cout << "Press Ctrl+C to exit...\n";
        
        // Busy polling (spin wait) - 실제 프로덕션 코드
        while (true) {
            bool processed = false;
            
            // 각 거래소의 이벤트 큐 처리
            auto process_events = [&processed](auto& client, const char* exchange_name) {
                auto& queue = client->event_queue();
                while (auto evt = queue.pop()) {
                    processed = true;
                    // 이벤트 처리 (여기서는 간단히 출력)
                    if (evt->is_ticker()) {
                        std::cout << "[Queue] " << exchange_name << " Price: " 
                                  << evt->ticker().price << "\n";
                    }
                }
            };
            
            process_events(upbit, "Upbit");
            process_events(binance, "Binance");
            process_events(bithumb, "Bithumb");
            process_events(mexc, "MEXC");
            
            // CPU 힌트: 이벤트가 없을 때만 pause 명령어 사용
            if (!processed) {
                __builtin_ia32_pause();  // x86 pause instruction
            }
        }
        
        // 정리
        ioc.stop();
        io_thread.join();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}