#include "arbitrage/exchange/mexc/websocket.hpp"
#include "arbitrage/common/json.hpp"
#include "arbitrage/common/simd_json_parser.hpp"
#include <chrono>
#include <algorithm>
#include <cctype>

namespace arbitrage {

MEXCWebSocket::MEXCWebSocket(net::io_context& ioc, ssl::context& ctx)
    : WebSocketClientBase(ioc, ctx, Exchange::MEXC) {
}

void MEXCWebSocket::subscribe_ticker(const std::vector<std::string>& symbols) {
    ticker_symbols_.from_vector(symbols);
}

void MEXCWebSocket::subscribe_orderbook(const std::vector<std::string>& symbols) {
    orderbook_symbols_.from_vector(symbols);
}

void MEXCWebSocket::subscribe_trade(const std::vector<std::string>& symbols) {
    trade_symbols_.from_vector(symbols);
}

void MEXCWebSocket::on_connected() {
    logger_->info("[MEXC] Connected, starting subscriptions...");

    ticker_idx_ = 0;
    orderbook_idx_ = 0;
    trade_idx_ = 0;
    ticker_done_ = ticker_symbols_.empty();
    orderbook_done_ = orderbook_symbols_.empty();
    trade_done_ = trade_symbols_.empty();

    send_subscriptions();
}

std::string MEXCWebSocket::build_subscribe_message() {
    // MEXC는 on_connected에서 처리
    return "";
}

// 심볼 형식 변환: XRPUSDT -> XRP_USDT
std::string MEXCWebSocket::convert_to_futures_symbol(const std::string& symbol) {
    if (symbol.find('_') != std::string::npos) {
        return symbol;
    }

    size_t usdt_pos = symbol.find("USDT");
    if (usdt_pos != std::string::npos && usdt_pos > 0) {
        return symbol.substr(0, usdt_pos) + "_USDT";
    }

    size_t usdc_pos = symbol.find("USDC");
    if (usdc_pos != std::string::npos && usdc_pos > 0) {
        return symbol.substr(0, usdc_pos) + "_USDC";
    }

    return symbol;
}

void MEXCWebSocket::send_subscriptions() {
    // 구독 메시지는 nlohmann/json으로 빌드 (write path)
    for (size_t i = 0; i < ticker_symbols_.size(); ++i) {
        std::string futures_symbol = convert_to_futures_symbol(ticker_symbols_.get(i));
        nlohmann::json sub_msg = {
            {"method", "sub.ticker"},
            {"param", {{"symbol", futures_symbol}}}
        };
        logger_->info("[MEXC] Subscribing to ticker: {}", futures_symbol);
        send_message(sub_msg.dump());
    }

    for (size_t i = 0; i < trade_symbols_.size(); ++i) {
        std::string futures_symbol = convert_to_futures_symbol(trade_symbols_.get(i));
        nlohmann::json sub_msg = {
            {"method", "sub.deal"},
            {"param", {{"symbol", futures_symbol}}}
        };
        logger_->info("[MEXC] Subscribing to deals: {}", futures_symbol);
        send_message(sub_msg.dump());
    }

    for (size_t i = 0; i < orderbook_symbols_.size(); ++i) {
        std::string futures_symbol = convert_to_futures_symbol(orderbook_symbols_.get(i));
        nlohmann::json sub_msg = {
            {"method", "sub.depth"},
            {"param", {{"symbol", futures_symbol}}}
        };
        logger_->info("[MEXC] Subscribing to depth: {}", futures_symbol);
        send_message(sub_msg.dump());
    }
}

// =============================================================================
// simdjson SIMD 가속 파싱 (AVX2/SSE4.2)
// =============================================================================

void MEXCWebSocket::parse_message(const std::string& message) {
    if (message.empty()) return;

    auto& parser = thread_local_simd_parser();
    simdjson::dom::element doc;
    if (parser.parse(message).get(doc) != simdjson::SUCCESS) {
        logger_->error("[MEXC] SIMD JSON parse error");
        return;
    }

    // Pong 응답 처리 (data == "pong")
    std::string_view data_str;
    if (doc["data"].get(data_str) == simdjson::SUCCESS && data_str == "pong") {
        logger_->debug("[MEXC] Received pong (data)");
        return;
    }

    std::string_view channel = simd_get_sv(doc["channel"]);
    if (channel.empty()) return;

    // Pong (channel)
    if (channel == "pong") {
        logger_->debug("[MEXC] Received pong (channel)");
        return;
    }

    // 구독 응답
    if (channel == "rs.sub.ticker" || channel == "rs.sub.deal" || channel == "rs.sub.depth") {
        logger_->info("[MEXC] Subscription confirmed: {}", channel);
        return;
    }

    // 데이터 메시지
    if (channel == "push.ticker") {
        parse_ticker(doc);
    } else if (channel == "push.deal") {
        parse_deal(doc);
    } else if (channel == "push.depth") {
        parse_depth(doc);
    }
}

void MEXCWebSocket::parse_ticker(simdjson::dom::element json) {
    simdjson::dom::element data;
    if (json["data"].get(data) != simdjson::SUCCESS) return;

    Ticker ticker;
    ticker.exchange = Exchange::MEXC;
    ticker.set_symbol(simd_get_sv(data["symbol"]));

    ticker.price = simd_get_double_or(data["lastPrice"]);

    double bid = simd_get_double_or(data["bid1"]);
    ticker.bid = (bid > 0.0) ? bid : ticker.price;

    double ask = simd_get_double_or(data["ask1"]);
    ticker.ask = (ask > 0.0) ? ask : ticker.price;

    ticker.volume_24h = simd_get_double_or(data["volume24"]);

    int64_t ts = simd_get_int64(data["timestamp"]);
    if (ts > 0) {
        ticker.timestamp_us = ts * 1000;
    } else {
        ticker.set_timestamp_now();
    }

    logger_->info("[MEXC] Ticker - Symbol: {}, Price: {}, Bid: {}, Ask: {}",
                  ticker.symbol, ticker.price, ticker.bid, ticker.ask);

    WebSocketEvent evt(WebSocketEvent::Type::Ticker, Exchange::MEXC, ticker);
    emit_event(std::move(evt));
}

void MEXCWebSocket::parse_deal(simdjson::dom::element json) {
    simdjson::dom::element data;
    if (json["data"].get(data) != simdjson::SUCCESS) return;

    std::string_view default_symbol = simd_get_sv(json["symbol"]);

    // data가 배열인 경우
    simdjson::dom::array arr;
    if (data.get(arr) == simdjson::SUCCESS) {
        for (auto deal : arr) {
            process_single_deal(deal, default_symbol);
        }
    } else {
        // 단일 객체
        process_single_deal(data, default_symbol);
    }
}

void MEXCWebSocket::process_single_deal(simdjson::dom::element deal, std::string_view default_symbol) {
    Ticker trade;
    trade.exchange = Exchange::MEXC;

    auto sym = simd_get_sv(deal["symbol"]);
    if (!sym.empty()) {
        trade.set_symbol(sym);
    } else {
        trade.set_symbol(default_symbol);
    }

    // 체결가
    double price = simd_get_double_or(deal["p"]);
    if (price == 0.0) price = simd_get_double_or(deal["price"]);
    trade.price = price;
    trade.bid = trade.price;
    trade.ask = trade.price;

    // 체결량
    double vol = simd_get_double_or(deal["v"]);
    if (vol == 0.0) vol = simd_get_double_or(deal["vol"]);
    trade.volume_24h = vol;

    // 타임스탬프
    int64_t ts = simd_get_int64(deal["t"]);
    if (ts == 0) ts = simd_get_int64(deal["ts"]);
    if (ts > 0) {
        // MEXC는 초 단위일 수 있음
        if (ts < 10000000000LL) ts *= 1000;  // 초 -> 밀리초
        trade.timestamp_us = ts * 1000;  // 밀리초 -> 마이크로초
    } else {
        trade.set_timestamp_now();
    }

    logger_->info("[MEXC] Deal - Symbol: {}, Price: {}, Vol: {}",
                  trade.symbol, trade.price, trade.volume_24h);

    WebSocketEvent trade_evt(WebSocketEvent::Type::Trade, Exchange::MEXC, trade);
    emit_event(std::move(trade_evt));

    // Ticker로도 emit (premium calculator용)
    Ticker ticker = trade;
    WebSocketEvent ticker_evt(WebSocketEvent::Type::Ticker, Exchange::MEXC, ticker);
    emit_event(std::move(ticker_evt));
}

void MEXCWebSocket::parse_depth(simdjson::dom::element json) {
    simdjson::dom::element data;
    if (json["data"].get(data) != simdjson::SUCCESS) return;

    OrderBook orderbook;
    orderbook.exchange = Exchange::MEXC;
    orderbook.clear();

    orderbook.set_symbol(simd_get_sv(json["symbol"]));

    // SIMD 가속 호가 파싱: MEXC는 [[price, qty], ...] 형식
    simdjson::dom::array asks;
    if (data["asks"].get(asks) == simdjson::SUCCESS) {
        for (auto ask : asks) {
            simdjson::dom::array pair;
            if (ask.get(pair) == simdjson::SUCCESS) {
                auto it = pair.begin();
                double price = simd_get_double(*it); ++it;
                double qty = simd_get_double(*it);
                orderbook.add_ask(price, qty);
            }
        }
    }

    simdjson::dom::array bids;
    if (data["bids"].get(bids) == simdjson::SUCCESS) {
        for (auto bid : bids) {
            simdjson::dom::array pair;
            if (bid.get(pair) == simdjson::SUCCESS) {
                auto it = pair.begin();
                double price = simd_get_double(*it); ++it;
                double qty = simd_get_double(*it);
                orderbook.add_bid(price, qty);
            }
        }
    }

    auto now = std::chrono::system_clock::now();
    orderbook.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    if (orderbook.bid_count > 0 && orderbook.ask_count > 0) {
        logger_->info("[MEXC] Depth - Best Bid: {}, Best Ask: {}",
                      orderbook.bids[0].price, orderbook.asks[0].price);
    }

    WebSocketEvent evt(WebSocketEvent::Type::OrderBook, Exchange::MEXC, orderbook);
    emit_event(std::move(evt));
}

std::chrono::seconds MEXCWebSocket::ping_interval() const {
    return std::chrono::seconds(10);
}

}  // namespace arbitrage
