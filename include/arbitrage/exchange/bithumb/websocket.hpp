#pragma once

#include "arbitrage/exchange/websocket_base.hpp"
#include "arbitrage/common/json.hpp"
#include <vector>
#include <string>

namespace arbitrage {

class BithumbWebSocket : public WebSocketClientBase {
public:
    BithumbWebSocket(net::io_context& ioc, ssl::context& ctx);
    
    // 구독 설정
    void subscribe_ticker(const std::vector<std::string>& symbols);
    void subscribe_orderbook(const std::vector<std::string>& symbols);
    void subscribe_trade(const std::vector<std::string>& symbols);
    
protected:
    // WebSocketClientBase 구현
    std::string build_subscribe_message() override;
    void parse_message(const std::string& message) override;
    
private:
    // 메시지 파서
    void parse_ticker(const nlohmann::json& data);
    void parse_single_ticker(const nlohmann::json& content);
    void parse_orderbook(const nlohmann::json& data);
    void parse_single_orderbook(const nlohmann::json& content);
    void parse_trade(const nlohmann::json& data);
    void send_additional_subscriptions();
    
    // 구독 정보
    std::vector<std::string> ticker_symbols_;
    std::vector<std::string> orderbook_symbols_;
    std::vector<std::string> trade_symbols_;
    
    // 구독 상태
    bool orderbook_subscribed_ = false;
    bool trade_subscribed_ = false;
};

}  // namespace arbitrage