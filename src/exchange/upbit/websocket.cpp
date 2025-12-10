#include "arbitrage/exchange/upbit/websocket.hpp"
#include "arbitrage/common/json.hpp"
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace arbitrage {

namespace {
    std::string generate_uuid() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        
        std::ostringstream oss;
        for (int i = 0; i < 8; i++) oss << std::hex << dis(gen);
        oss << "-";
        for (int i = 0; i < 4; i++) oss << std::hex << dis(gen);
        oss << "-4"; // version 4
        for (int i = 0; i < 3; i++) oss << std::hex << dis(gen);
        oss << "-";
        oss << std::hex << ((dis(gen) & 0x3) | 0x8); // variant
        for (int i = 0; i < 3; i++) oss << std::hex << dis(gen);
        oss << "-";
        for (int i = 0; i < 12; i++) oss << std::hex << dis(gen);
        
        return oss.str();
    }
}

UpbitWebSocket::UpbitWebSocket(net::io_context& ioc, ssl::context& ctx)
    : WebSocketClientBase(ioc, ctx, Exchange::Upbit)
    , ticket_id_(generate_uuid()) {
}

void UpbitWebSocket::subscribe_ticker(const std::vector<std::string>& symbols) {
    ticker_symbols_ = symbols;
}

void UpbitWebSocket::subscribe_orderbook(const std::vector<std::string>& symbols) {
    orderbook_symbols_ = symbols;
}

void UpbitWebSocket::subscribe_trade(const std::vector<std::string>& symbols) {
    trade_symbols_ = symbols;
}

std::string UpbitWebSocket::build_subscribe_message() {
    nlohmann::json messages = nlohmann::json::array();
    
    // 티켓 메시지 (필수)
    messages.push_back({
        {"ticket", ticket_id_}
    });
    
    // 시세 구독
    if (!ticker_symbols_.empty()) {
        messages.push_back({
            {"type", "ticker"},
            {"codes", ticker_symbols_},
            {"isOnlyRealtime", true}
        });
    }
    
    // 호가 구독
    if (!orderbook_symbols_.empty()) {
        messages.push_back({
            {"type", "orderbook"},
            {"codes", orderbook_symbols_},
            {"isOnlyRealtime", true}
        });
    }
    
    // 체결 구독
    if (!trade_symbols_.empty()) {
        messages.push_back({
            {"type", "trade"},
            {"codes", trade_symbols_},
            {"isOnlyRealtime", true}
        });
    }
    
    return messages.dump();
}

void UpbitWebSocket::parse_message(const std::string& message) {
    try {
        auto json = nlohmann::json::parse(message);
        
        if (!json.contains("type")) {
            logger_->warn("[Upbit] Message has no type field");
            return;
        }
        
        std::string type = json["type"].get<std::string>();
        
        if (type == "ticker") {
            parse_ticker(json);
        } else if (type == "orderbook") {
            parse_orderbook(json);
        } else if (type == "trade") {
            parse_trade(json);
        } else {
            logger_->debug("[Upbit] Unknown message type: {}", type);
        }
        
    } catch (const std::exception& e) {
        logger_->error("[Upbit] Parse error: {}", e.what());
    }
}

void UpbitWebSocket::parse_ticker(const nlohmann::json& data) {
    Ticker ticker;
    ticker.exchange = Exchange::Upbit;
    ticker.symbol = data["code"];
    ticker.price = data["trade_price"];
    ticker.bid = data.value("bid_price", 0.0);
    ticker.ask = data.value("ask_price", 0.0);
    ticker.volume_24h = data["acc_trade_volume_24h"];
    
    // Upbit는 타임스탬프를 밀리초로 제공
    int64_t timestamp_ms = data["timestamp"];
    ticker.timestamp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(timestamp_ms)
    );
    
    WebSocketEvent evt{
        WebSocketEvent::Type::Ticker,
        Exchange::Upbit,
        ticker,
        ""
    };
    
    emit_event(std::move(evt));
}

void UpbitWebSocket::parse_orderbook(const nlohmann::json& data) {
    OrderBook orderbook;
    orderbook.exchange = Exchange::Upbit;
    orderbook.symbol = data["code"];
    
    // 호가 데이터 파싱
    auto orderbook_units = data["orderbook_units"];
    
    for (const auto& unit : orderbook_units) {
        // 매도 호가
        PriceLevel ask;
        ask.price = unit["ask_price"];
        ask.quantity = unit["ask_size"];
        orderbook.asks.push_back(ask);
        
        // 매수 호가
        PriceLevel bid;
        bid.price = unit["bid_price"];
        bid.quantity = unit["bid_size"];
        orderbook.bids.push_back(bid);
    }
    
    // 타임스탬프
    int64_t timestamp_ms = data["timestamp"];
    orderbook.timestamp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(timestamp_ms)
    );
    
    WebSocketEvent evt{
        WebSocketEvent::Type::OrderBook,
        Exchange::Upbit,
        orderbook,
        ""
    };
    
    emit_event(std::move(evt));
}

void UpbitWebSocket::parse_trade(const nlohmann::json& data) {
    // Trade 이벤트는 현재 사용하지 않으므로 구현 생략
    // 필요시 추가 구현
}

}  // namespace arbitrage