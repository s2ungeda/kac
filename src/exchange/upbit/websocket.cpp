#include "arbitrage/exchange/upbit/websocket.hpp"
#include "arbitrage/common/json.hpp"
#include "arbitrage/common/simd_json_parser.hpp"
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
    ticker_symbols_.from_vector(symbols);
}

void UpbitWebSocket::subscribe_orderbook(const std::vector<std::string>& symbols) {
    orderbook_symbols_.from_vector(symbols);
}

void UpbitWebSocket::subscribe_trade(const std::vector<std::string>& symbols) {
    trade_symbols_.from_vector(symbols);
}

std::string UpbitWebSocket::build_subscribe_message() {
    // 구독 메시지는 nlohmann/json으로 빌드 (write path)
    nlohmann::json messages = nlohmann::json::array();

    messages.push_back({
        {"ticket", ticket_id_}
    });

    if (!ticker_symbols_.empty()) {
        nlohmann::json codes = nlohmann::json::array();
        for (size_t i = 0; i < ticker_symbols_.size(); ++i) {
            codes.push_back(ticker_symbols_.get(i));
        }
        messages.push_back({
            {"type", "ticker"},
            {"codes", codes},
            {"isOnlyRealtime", true}
        });
    }

    if (!orderbook_symbols_.empty()) {
        nlohmann::json codes = nlohmann::json::array();
        for (size_t i = 0; i < orderbook_symbols_.size(); ++i) {
            codes.push_back(orderbook_symbols_.get(i));
        }
        messages.push_back({
            {"type", "orderbook"},
            {"codes", codes},
            {"isOnlyRealtime", true}
        });
    }

    if (!trade_symbols_.empty()) {
        nlohmann::json codes = nlohmann::json::array();
        for (size_t i = 0; i < trade_symbols_.size(); ++i) {
            codes.push_back(trade_symbols_.get(i));
        }
        messages.push_back({
            {"type", "trade"},
            {"codes", codes},
            {"isOnlyRealtime", true}
        });
    }

    return messages.dump();
}

// =============================================================================
// 필드 매핑 + simdjson SIMD 가속 파싱
// =============================================================================

namespace {
    const TickerFieldMap UPBIT_TICKER_MAP {
        "code", "trade_price", "bid_price", "ask_price",
        "acc_trade_volume_24h", "timestamp", 1000, false
    };
    const OrderBookFieldMap UPBIT_OB_MAP {
        "code", "timestamp", 1000,
        OrderBookFieldMap::OBJECTS,
        "orderbook_units", "ask_price", "ask_size", "bid_price", "bid_size", false,
        "", ""
    };
    const TradeFieldMap UPBIT_TRADE_MAP {
        "code", "trade_price", "trade_volume", "trade_timestamp", 1000
    };
}

void UpbitWebSocket::parse_message(const std::string& message) {
    auto& parser = thread_local_simd_parser();
    simdjson::dom::element doc;
    if (parser.parse(message).get(doc) != simdjson::SUCCESS) {
        logger_->error("[Upbit] SIMD JSON parse error");
        return;
    }

    std::string_view type = simd_get_sv(doc["type"]);
    if (type.empty()) return;

    if (type == "ticker") {
        parse_ticker(doc);
    } else if (type == "orderbook") {
        parse_orderbook(doc);
    } else if (type == "trade") {
        parse_trade(doc);
    }
}

void UpbitWebSocket::parse_ticker(simdjson::dom::element data) {
    auto ticker = make_ticker(data, UPBIT_TICKER_MAP);
    logger_->info("[Upbit] Ticker - Symbol: {}, Price: {} KRW",
                  ticker.symbol, static_cast<int>(ticker.price));
    emit_event({WebSocketEvent::Type::Ticker, Exchange::Upbit, ticker});
}

void UpbitWebSocket::parse_orderbook(simdjson::dom::element data) {
    auto orderbook = make_orderbook(data, UPBIT_OB_MAP);

    logger_->info("[Upbit] OrderBook - Symbol: {}, Bids: {}, Asks: {}, BestBid: {}, BestAsk: {}",
                  orderbook.symbol, orderbook.bid_count, orderbook.ask_count,
                  orderbook.best_bid(), orderbook.best_ask());
    emit_event({WebSocketEvent::Type::OrderBook, Exchange::Upbit, orderbook});
}

void UpbitWebSocket::parse_trade(simdjson::dom::element data) {
    auto trade = make_trade(data, UPBIT_TRADE_MAP);
    logger_->info("[Upbit] Trade - Code: {}, Price: {} KRW, Vol: {}",
                  trade.symbol, trade.price, trade.volume_24h);
    emit_event({WebSocketEvent::Type::Trade, Exchange::Upbit, trade});
}

}  // namespace arbitrage
