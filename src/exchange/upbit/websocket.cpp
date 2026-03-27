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
// simdjson SIMD 가속 파싱 (AVX2/SSE4.2)
// =============================================================================

void UpbitWebSocket::parse_message(const std::string& message) {
    auto& parser = thread_local_simd_parser();
    simdjson::dom::element doc;
    if (parser.parse(message).get(doc) != simdjson::SUCCESS) {
        logger_->error("[Upbit] SIMD JSON parse error");
        return;
    }

    std::string_view type = simd_get_sv(doc["type"]);
    if (type.empty()) {
        logger_->warn("[Upbit] Message has no type field");
        return;
    }

    if (type == "ticker") {
        parse_ticker(doc);
    } else if (type == "orderbook") {
        parse_orderbook(doc);
    } else if (type == "trade") {
        parse_trade(doc);
    } else {
        logger_->debug("[Upbit] Unknown message type: {}", type);
    }
}

void UpbitWebSocket::parse_ticker(simdjson::dom::element data) {
    Ticker ticker;
    ticker.exchange = Exchange::Upbit;
    ticker.set_symbol(simd_get_sv(data["code"]));
    ticker.price = simd_get_double_or(data["trade_price"]);
    ticker.bid = simd_get_double_or(data["bid_price"]);
    ticker.ask = simd_get_double_or(data["ask_price"]);
    ticker.volume_24h = simd_get_double_or(data["acc_trade_volume_24h"]);
    ticker.timestamp_us = simd_get_int64(data["timestamp"]) * 1000;

    logger_->info("[Upbit] Ticker - Symbol: {}, Price: {} KRW",
                  ticker.symbol, static_cast<int>(ticker.price));

    WebSocketEvent evt(WebSocketEvent::Type::Ticker, Exchange::Upbit, ticker);
    emit_event(std::move(evt));
}

void UpbitWebSocket::parse_orderbook(simdjson::dom::element data) {
    OrderBook orderbook;
    orderbook.exchange = Exchange::Upbit;
    orderbook.clear();
    orderbook.set_symbol(simd_get_sv(data["code"]));

    // SIMD 가속 호가 파싱: orderbook_units 배열 순회
    simdjson::dom::array units;
    if (data["orderbook_units"].get(units) == simdjson::SUCCESS) {
        for (auto unit : units) {
            orderbook.add_ask(
                simd_get_double_or(unit["ask_price"]),
                simd_get_double_or(unit["ask_size"])
            );
            orderbook.add_bid(
                simd_get_double_or(unit["bid_price"]),
                simd_get_double_or(unit["bid_size"])
            );
        }
    }

    orderbook.timestamp_us = simd_get_int64(data["timestamp"]) * 1000;

    if (orderbook.bid_count >= 5) {
        logger_->debug("[Upbit] {} Top 5 Bids: [{:.0f}@{:.4f}, {:.0f}@{:.4f}, {:.0f}@{:.4f}, {:.0f}@{:.4f}, {:.0f}@{:.4f}]",
                      orderbook.symbol,
                      orderbook.bids[0].price, orderbook.bids[0].quantity,
                      orderbook.bids[1].price, orderbook.bids[1].quantity,
                      orderbook.bids[2].price, orderbook.bids[2].quantity,
                      orderbook.bids[3].price, orderbook.bids[3].quantity,
                      orderbook.bids[4].price, orderbook.bids[4].quantity);
        logger_->debug("[Upbit] {} Top 5 Asks: [{:.0f}@{:.4f}, {:.0f}@{:.4f}, {:.0f}@{:.4f}, {:.0f}@{:.4f}, {:.0f}@{:.4f}]",
                      orderbook.symbol,
                      orderbook.asks[0].price, orderbook.asks[0].quantity,
                      orderbook.asks[1].price, orderbook.asks[1].quantity,
                      orderbook.asks[2].price, orderbook.asks[2].quantity,
                      orderbook.asks[3].price, orderbook.asks[3].quantity,
                      orderbook.asks[4].price, orderbook.asks[4].quantity);
    }

    logger_->info("[Upbit] OrderBook - Symbol: {}, Bids: {}, Asks: {}, BestBid: {}, BestAsk: {}",
                  orderbook.symbol, orderbook.bid_count, orderbook.ask_count,
                  orderbook.best_bid(), orderbook.best_ask());

    WebSocketEvent evt(WebSocketEvent::Type::OrderBook, Exchange::Upbit, orderbook);
    emit_event(std::move(evt));
}

void UpbitWebSocket::parse_trade(simdjson::dom::element data) {
    Ticker trade;
    trade.exchange = Exchange::Upbit;
    trade.set_symbol(simd_get_sv(data["code"]));
    trade.price = simd_get_double_or(data["trade_price"]);
    trade.bid = trade.price;
    trade.ask = trade.price;
    trade.volume_24h = simd_get_double_or(data["trade_volume"]);
    trade.timestamp_us = simd_get_int64(data["trade_timestamp"]) * 1000;

    logger_->info("[Upbit] Trade - Code: {}, Price: {} KRW, Vol: {}",
                  trade.symbol, trade.price, trade.volume_24h);

    WebSocketEvent evt(WebSocketEvent::Type::Trade, Exchange::Upbit, trade);
    emit_event(std::move(evt));
}

}  // namespace arbitrage
