#include <iostream>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include "arbitrage/exchange/mexc/websocket.hpp"
#include "arbitrage/common/logger.hpp"

using namespace arbitrage;

int main() {
    try {
        // Logger 초기화
        Logger::init("logs", LogLevel::Debug, LogLevel::Debug);
        
        // IO Context와 SSL Context 생성
        boost::asio::io_context ioc;
        boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tlsv12_client};
        ssl_ctx.set_default_verify_paths();
        
        auto mexc = std::make_shared<MEXCWebSocket>(ioc, ssl_ctx);
        
        // 이벤트 핸들러 설정
        mexc->on_event([](const WebSocketEvent& evt) {
            switch (evt.type) {
                case WebSocketEvent::Type::Connected:
                    std::cout << "[MEXC] Connected\n";
                    break;
                case WebSocketEvent::Type::Error:
                    std::cout << "[MEXC] Error: " << evt.error_message << "\n";
                    break;
                default:
                    break;
            }
        });
        
        // 델파이와 정확히 같은 채널만 구독
        mexc->subscribe_trade({"BTCUSDT"});
        
        std::cout << "Connecting to MEXC...\n";
        mexc->connect("wbs-api.mexc.com", "443", "/ws");
        
        // IO 실행
        ioc.run();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}