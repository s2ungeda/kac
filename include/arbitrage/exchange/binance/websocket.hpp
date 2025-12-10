#pragma once

#include "arbitrage/exchange/websocket_base.hpp"
#include <vector>
#include <string>

#include "arbitrage/common/json.hpp"

namespace arbitrage {

class BinanceWebSocket : public WebSocketClientBase {
public:
    BinanceWebSocket(net::io_context& ioc, ssl::context& ctx);
    
    // 구독 설정
    void subscribe_ticker(const std::vector<std::string>& symbols);
    void subscribe_orderbook(const std::vector<std::string>& symbols, int depth = 10);
    void subscribe_trade(const std::vector<std::string>& symbols);
    
    // Binance는 stream 파라미터로 구독하므로 connect 오버라이드
    void connect_with_streams();
    
protected:
    // WebSocketClientBase 구현
    std::string build_subscribe_message() override;
    void parse_message(const std::string& message) override;
    std::chrono::seconds ping_interval() const override { 
        // Binance는 자동으로 ping/pong을 처리
        return std::chrono::seconds(0); 
    }
    
private:
    // 메시지 파서
    void parse_ticker(const nlohmann::json& data);
    void parse_orderbook(const nlohmann::json& data);
    void parse_trade(const nlohmann::json& data);
    
    // 스트림 목록
    std::vector<std::string> streams_;
    int orderbook_depth_{10};
    
    // 구독 ID
    int subscribe_id_{1};
};

}  // namespace arbitrage