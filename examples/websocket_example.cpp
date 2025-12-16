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

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  --help        Show this help message\n";
    std::cout << "  --upbit       Test only Upbit\n";
    std::cout << "  --binance     Test only Binance\n";
    std::cout << "  --bithumb     Test only Bithumb\n";
    std::cout << "  --mexc        Test only MEXC\n";
    std::cout << "  (no options)  Test all exchanges\n\n";
}

int main(int argc, char* argv[]) {
    try {
        // 명령행 인자 파싱
        bool test_upbit = false;
        bool test_binance = false;
        bool test_bithumb = false;
        bool test_mexc = false;
        bool test_all = true;
        
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                print_usage(argv[0]);
                return 0;
            } else if (arg == "--upbit") {
                test_upbit = true;
                test_all = false;
            } else if (arg == "--binance") {
                test_binance = true;
                test_all = false;
            } else if (arg == "--bithumb") {
                test_bithumb = true;
                test_all = false;
            } else if (arg == "--mexc") {
                test_mexc = true;
                test_all = false;
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage(argv[0]);
                return 1;
            }
        }
        
        // 모든 거래소 테스트인 경우
        if (test_all) {
            test_upbit = test_binance = test_bithumb = test_mexc = true;
        }
        
        // Logger 초기화 (디버그 레벨로 설정)
        Logger::init("logs", LogLevel::Info, LogLevel::Info);
        
        // IO Context와 SSL Context 생성
        boost::asio::io_context ioc;
        boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tlsv12_client};
        ssl_ctx.set_default_verify_paths();
        
        // WebSocket 클라이언트 생성 (필요한 것만)
        std::shared_ptr<UpbitWebSocket> upbit;
        std::shared_ptr<BinanceWebSocket> binance;
        std::shared_ptr<BithumbWebSocket> bithumb;
        std::shared_ptr<MEXCWebSocket> mexc;
        
        if (test_upbit) {
            upbit = std::make_shared<UpbitWebSocket>(ioc, ssl_ctx);
            std::cout << "Testing Upbit WebSocket\n";
        }
        if (test_binance) {
            binance = std::make_shared<BinanceWebSocket>(ioc, ssl_ctx);
            std::cout << "Testing Binance WebSocket\n";
        }
        if (test_bithumb) {
            bithumb = std::make_shared<BithumbWebSocket>(ioc, ssl_ctx);
            std::cout << "Testing Bithumb WebSocket\n";
        }
        if (test_mexc) {
            mexc = std::make_shared<MEXCWebSocket>(ioc, ssl_ctx);
            std::cout << "Testing MEXC WebSocket\n";
        }
        
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
                    
                case WebSocketEvent::Type::OrderBook: {
                    const auto& ob = evt.orderbook();
                    std::cout << "[" << exchange_name(evt.exchange) 
                              << "] OrderBook: " 
                              << ob.symbol 
                              << " Best Bid: " << ob.best_bid()
                              << " Best Ask: " << ob.best_ask()
                              << "\n";
                    break;
                }
                
                case WebSocketEvent::Type::Trade:
                    std::cout << "[" << exchange_name(evt.exchange) 
                              << "] Trade: " 
                              << evt.ticker().symbol << " @ " 
                              << evt.ticker().price 
                              << " (실시간 체결가)\n";
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
        if (upbit) upbit->on_event(event_handler);
        if (binance) binance->on_event(event_handler);
        if (bithumb) bithumb->on_event(event_handler);
        if (mexc) mexc->on_event(event_handler);
        
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
        if (test_upbit) {
            std::cout << "  Upbit: ";
            for (const auto& s : upbit_symbols) std::cout << s << " ";
            std::cout << "\n";
        }
        if (test_binance) {
            std::cout << "  Binance: ";
            for (const auto& s : binance_symbols) std::cout << s << " ";
            std::cout << "\n";
        }
        if (test_bithumb) {
            std::cout << "  Bithumb: ";
            for (const auto& s : bithumb_symbols) std::cout << s << " ";
            std::cout << "\n";
        }
        if (test_mexc) {
            std::cout << "  MEXC: ";
            for (const auto& s : mexc_symbols) std::cout << s << " ";
            std::cout << "\n";
        }
        std::cout << "\n";
        
        // 구독 설정
        if (upbit) {
            upbit->subscribe_ticker(upbit_symbols);
            upbit->subscribe_orderbook(upbit_symbols);
            upbit->subscribe_trade(upbit_symbols);  // 실시간 체결가
        }
        
        if (binance) {
            binance->subscribe_ticker(binance_symbols);
            binance->subscribe_orderbook(binance_symbols, 10);
            binance->subscribe_trade(binance_symbols);  // 실시간 체결가
        }
        
        if (bithumb) {
            bithumb->subscribe_ticker(bithumb_symbols);
            bithumb->subscribe_orderbook(bithumb_symbols);
            bithumb->subscribe_trade(bithumb_symbols);  // 실시간 체결가
        }
        
        if (mexc) {
            mexc->subscribe_ticker(mexc_symbols);
            mexc->subscribe_orderbook(mexc_symbols);  // MEXC orderbook 활성화
            mexc->subscribe_trade(mexc_symbols);  // 실시간 체결가
        }
        
        // 연결
        std::cout << "Connecting to exchanges...\n";
        
        if (upbit) upbit->connect("api.upbit.com", "443", "/websocket/v1");
        if (binance) binance->connect_with_streams();  // Combined Stream 사용
        if (bithumb) bithumb->connect("pubwss.bithumb.com", "443", "/pub/ws");
        if (mexc) mexc->connect("wbs-api.mexc.com", "443", "/ws");
        
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
                if (!client) return;
                auto& queue = client->event_queue();
                WebSocketEvent evt;
                while (queue.pop(evt)) {
                    processed = true;
                    // 이벤트 처리 (큐에서 읽은 이벤트는 이미 출력되었으므로 여기서는 무시)
                }
            };
            
            if (upbit) process_events(upbit, "Upbit");
            if (binance) process_events(binance, "Binance");
            if (bithumb) process_events(bithumb, "Bithumb");
            if (mexc) process_events(mexc, "MEXC");
            
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