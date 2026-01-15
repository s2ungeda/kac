#pragma once

#include "arbitrage/exchange/websocket_base.hpp"
#include "arbitrage/common/json.hpp"
#include <vector>
#include <string>

namespace arbitrage {

// 빗썸 v2 WebSocket API
// URL: wss://ws-api.bithumb.com/websocket/v1
class BithumbWebSocket : public WebSocketClientBase {
public:
    BithumbWebSocket(net::io_context& ioc, ssl::context& ctx);

    // 구독 설정 (심볼은 XRP_KRW 또는 KRW-XRP 형식 모두 지원)
    void subscribe_ticker(const std::vector<std::string>& symbols);
    void subscribe_orderbook(const std::vector<std::string>& symbols);
    void subscribe_trade(const std::vector<std::string>& symbols);

protected:
    // WebSocketClientBase 구현
    std::string build_subscribe_message() override;
    void parse_message(const std::string& message) override;

private:
    // 심볼 변환: XRP_KRW -> KRW-XRP
    std::string convert_to_v2_code(const std::string& symbol);

    // v2 API 메시지 파서
    void parse_trade_v2(const nlohmann::json& json);
    void parse_ticker_v2(const nlohmann::json& json);
    void parse_orderbook_v2(const nlohmann::json& json);

    // 구독 코드 (KRW-XRP 형식)
    std::vector<std::string> ticker_codes_;
    std::vector<std::string> orderbook_codes_;
    std::vector<std::string> trade_codes_;
};

}  // namespace arbitrage
