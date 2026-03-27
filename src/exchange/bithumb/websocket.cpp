#include "arbitrage/exchange/bithumb/websocket.hpp"
#include "arbitrage/common/json.hpp"
#include "arbitrage/common/simd_json_parser.hpp"
#include <chrono>
#include <map>
#include <algorithm>

namespace arbitrage {

BithumbWebSocket::BithumbWebSocket(net::io_context& ioc, ssl::context& ctx)
    : WebSocketClientBase(ioc, ctx, Exchange::Bithumb) {
}

void BithumbWebSocket::subscribe_ticker(const std::vector<std::string>& symbols) {
    for (const auto& sym : symbols) {
        ticker_codes_.add(convert_to_v2_code(sym));
    }
}

void BithumbWebSocket::subscribe_orderbook(const std::vector<std::string>& symbols) {
    for (const auto& sym : symbols) {
        orderbook_codes_.add(convert_to_v2_code(sym));
    }
}

void BithumbWebSocket::subscribe_trade(const std::vector<std::string>& symbols) {
    for (const auto& sym : symbols) {
        trade_codes_.add(convert_to_v2_code(sym));
    }
}

// 심볼 변환: XRP_KRW -> KRW-XRP
std::string BithumbWebSocket::convert_to_v2_code(const std::string& symbol) {
    if (symbol.find("KRW-") == 0) {
        return symbol;
    }

    size_t pos = symbol.find("_KRW");
    if (pos != std::string::npos) {
        return "KRW-" + symbol.substr(0, pos);
    }

    pos = symbol.find("KRW");
    if (pos != std::string::npos && pos > 0) {
        return "KRW-" + symbol.substr(0, pos);
    }

    return symbol;
}

std::string BithumbWebSocket::build_subscribe_message() {
    // 구독 메시지는 nlohmann/json으로 빌드 (write path)
    nlohmann::json msg = nlohmann::json::array();

    msg.push_back({{"ticket", "arbitrage-cpp"}});

    if (!trade_codes_.empty()) {
        nlohmann::json codes = nlohmann::json::array();
        for (size_t i = 0; i < trade_codes_.size(); ++i) {
            codes.push_back(trade_codes_.get(i));
        }
        msg.push_back({
            {"type", "trade"},
            {"codes", codes},
            {"isOnlyRealtime", true}
        });
    }

    if (!ticker_codes_.empty()) {
        nlohmann::json codes = nlohmann::json::array();
        for (size_t i = 0; i < ticker_codes_.size(); ++i) {
            codes.push_back(ticker_codes_.get(i));
        }
        msg.push_back({
            {"type", "ticker"},
            {"codes", codes},
            {"isOnlyRealtime", true}
        });
    }

    if (!orderbook_codes_.empty()) {
        nlohmann::json codes = nlohmann::json::array();
        for (size_t i = 0; i < orderbook_codes_.size(); ++i) {
            codes.push_back(orderbook_codes_.get(i));
        }
        msg.push_back({
            {"type", "orderbook"},
            {"codes", codes},
            {"isOnlyRealtime", true}
        });
    }

    msg.push_back({{"format", "DEFAULT"}});

    return msg.dump();
}

// =============================================================================
// simdjson SIMD 가속 파싱 (AVX2/SSE4.2)
// =============================================================================

void BithumbWebSocket::parse_message(const std::string& message) {
    auto& parser = thread_local_simd_parser();
    simdjson::dom::element doc;
    if (parser.parse(message).get(doc) != simdjson::SUCCESS) {
        logger_->error("[Bithumb] SIMD JSON parse error");
        return;
    }

    // 에러 응답 처리
    if (simd_has_field(doc, "error")) {
        logger_->error("[Bithumb] Error response");
        return;
    }

    // type 필드 확인
    std::string_view type = simd_get_sv(doc["type"]);
    if (type.empty()) {
        std::string_view status = simd_get_sv(doc["status"]);
        if (status == "UP") {
            logger_->info("[Bithumb] Connection status: UP");
        }
        return;
    }

    if (type == "trade") {
        parse_trade_v2(doc);
    } else if (type == "ticker") {
        parse_ticker_v2(doc);
    } else if (type == "orderbook") {
        parse_orderbook_v2(doc);
    }
}

void BithumbWebSocket::parse_trade_v2(simdjson::dom::element json) {
    Ticker trade;
    trade.exchange = Exchange::Bithumb;

    trade.set_symbol(simd_get_sv(json["code"]));

    double trade_price = simd_get_double_or(json["trade_price"]);
    if (trade_price > 0.0) {
        trade.price = trade_price;
        trade.bid = trade.price;
        trade.ask = trade.price;
    }

    trade.volume_24h = simd_get_double_or(json["trade_volume"]);

    int64_t ts = simd_get_int64(json["trade_timestamp"]);
    if (ts > 0) {
        trade.timestamp_us = ts * 1000;
    } else {
        auto now = std::chrono::system_clock::now();
        trade.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
    }

    // SNAPSHOT은 무시 (REALTIME만 처리)
    std::string_view stream_type = simd_get_sv(json["stream_type"]);
    if (stream_type == "SNAPSHOT") {
        return;
    }

    logger_->info("[Bithumb] Trade - Code: {}, Price: {} KRW, Vol: {}",
                  trade.symbol, trade.price, trade.volume_24h);

    WebSocketEvent trade_evt(WebSocketEvent::Type::Trade, Exchange::Bithumb, trade);
    emit_event(std::move(trade_evt));

    // Ticker로도 emit (premium calculator용)
    Ticker ticker = trade;
    WebSocketEvent ticker_evt(WebSocketEvent::Type::Ticker, Exchange::Bithumb, ticker);
    emit_event(std::move(ticker_evt));
}

void BithumbWebSocket::parse_ticker_v2(simdjson::dom::element json) {
    Ticker ticker;
    ticker.exchange = Exchange::Bithumb;

    ticker.set_symbol(simd_get_sv(json["code"]));

    ticker.price = simd_get_double_or(json["trade_price"]);

    double bid = simd_get_double_or(json["best_bid_price"]);
    ticker.bid = (bid > 0.0) ? bid : ticker.price;

    double ask = simd_get_double_or(json["best_ask_price"]);
    ticker.ask = (ask > 0.0) ? ask : ticker.price;

    ticker.volume_24h = simd_get_double_or(json["acc_trade_volume_24h"]);
    ticker.set_timestamp_now();

    logger_->info("[Bithumb] Ticker - Code: {}, Price: {} KRW",
                  ticker.symbol, ticker.price);

    WebSocketEvent evt(WebSocketEvent::Type::Ticker, Exchange::Bithumb, ticker);
    emit_event(std::move(evt));
}

void BithumbWebSocket::parse_orderbook_v2(simdjson::dom::element json) {
    OrderBook orderbook;
    orderbook.exchange = Exchange::Bithumb;
    orderbook.clear();

    orderbook.set_symbol(simd_get_sv(json["code"]));

    // SIMD 가속 호가 파싱: orderbook_units 배열 순회
    simdjson::dom::array units;
    if (json["orderbook_units"].get(units) == simdjson::SUCCESS) {
        for (auto unit : units) {
            double ask_price = simd_get_double_or(unit["ask_price"]);
            double ask_size = simd_get_double_or(unit["ask_size"]);
            if (ask_price > 0.0) {
                orderbook.add_ask(ask_price, ask_size);
            }

            double bid_price = simd_get_double_or(unit["bid_price"]);
            double bid_size = simd_get_double_or(unit["bid_size"]);
            if (bid_price > 0.0) {
                orderbook.add_bid(bid_price, bid_size);
            }
        }
    }

    auto now = std::chrono::system_clock::now();
    orderbook.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    if (orderbook.bid_count > 0 && orderbook.ask_count > 0) {
        logger_->info("[Bithumb] OrderBook - Best Bid: {}, Best Ask: {}",
                      orderbook.bids[0].price, orderbook.asks[0].price);
    }

    WebSocketEvent evt(WebSocketEvent::Type::OrderBook, Exchange::Bithumb, orderbook);
    emit_event(std::move(evt));
}

}  // namespace arbitrage
