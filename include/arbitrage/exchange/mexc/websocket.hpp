#pragma once

#include "arbitrage/exchange/websocket_base.hpp"
#include <simdjson.h>
#include <vector>
#include <string>

namespace arbitrage {

// MEXC Futures WebSocket 클라이언트
// URL: wss://contract.mexc.com/edge
class MEXCWebSocket : public WebSocketClientBase {
public:
    MEXCWebSocket(net::io_context& ioc, ssl::context& ctx);

    // 구독 설정
    void subscribe_ticker(const std::vector<std::string>& symbols);
    void subscribe_orderbook(const std::vector<std::string>& symbols);
    void subscribe_trade(const std::vector<std::string>& symbols);

protected:
    // WebSocketClientBase 구현
    std::string build_subscribe_message() override;
    void parse_message(const std::string& message) override;
    std::chrono::seconds ping_interval() const override;
    void on_connected() override;

private:
    // 심볼 변환: XRPUSDT -> XRP_USDT
    std::string convert_to_futures_symbol(const std::string& symbol);

    // 구독 전송
    void send_subscriptions();

    // SIMD 가속 메시지 파서
    void parse_ticker(simdjson::dom::element json);
    void parse_deal(simdjson::dom::element json);
    void parse_depth(simdjson::dom::element json);
    void process_single_deal(simdjson::dom::element deal, std::string_view default_symbol);

    // 구독 정보 (고정 크기 배열, Zero-Copy)
    SymbolList ticker_symbols_;
    SymbolList orderbook_symbols_;
    SymbolList trade_symbols_;

    // 구독 ID
    int subscribe_id_{1};

    // 구독 상태 추적
    size_t ticker_idx_{0};
    size_t orderbook_idx_{0};
    size_t trade_idx_{0};
    bool ticker_done_{false};
    bool orderbook_done_{false};
    bool trade_done_{false};
};

}  // namespace arbitrage
