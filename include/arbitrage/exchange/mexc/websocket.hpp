#pragma once

#include "arbitrage/exchange/websocket_base.hpp"
#include "arbitrage/common/json.hpp"
#include <vector>
#include <string>

namespace arbitrage {

class MEXCWebSocket : public WebSocketClientBase {
public:
    MEXCWebSocket(net::io_context& ioc, ssl::context& ctx);
    
    // 구독 설정
    void subscribe_ticker(const std::vector<std::string>& symbols);
    void subscribe_orderbook(const std::vector<std::string>& symbols);
    void subscribe_trade(const std::vector<std::string>& symbols);
    
    // MEXC는 다른 엔드포인트 사용
    void connect_orderbook(const std::string& host, const std::string& port, const std::string& path);
    
protected:
    // WebSocketClientBase 구현
    std::string build_subscribe_message() override;
    void parse_message(const std::string& message) override;
    std::chrono::seconds ping_interval() const override { 
        return std::chrono::seconds(20); 
    }
    
private:
    // 메시지 파서
    void parse_ticker(const nlohmann::json& data);
    void parse_orderbook(const nlohmann::json& data);
    void parse_trade(const nlohmann::json& data);
    
    // Protobuf parsing
    void parse_ticker_protobuf(const std::string& data);
    void parse_orderbook_protobuf(const std::string& data, const std::string& symbol);
    void parse_trade_protobuf(const std::string& data, const std::string& symbol);
    
    // 구독 정보
    std::vector<std::string> ticker_symbols_;
    std::vector<std::string> orderbook_symbols_;
    std::vector<std::string> trade_symbols_;
    
    // 구독 ID
    int subscribe_id_{1};
    
    // 지연된 구독을 위한 타이머
    void on_connected() override;
    void send_delayed_subscriptions();
    
    // 구독 상태 추적
    size_t ticker_idx_{0};
    size_t orderbook_idx_{0};
    size_t trade_idx_{0};
    bool ticker_done_{false};
    bool orderbook_done_{false};
    bool trade_done_{false};
};

}  // namespace arbitrage