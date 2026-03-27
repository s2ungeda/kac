#include "arbitrage/exchange/binance/websocket.hpp"
#include "arbitrage/common/json.hpp"
#include "arbitrage/common/simd_json_parser.hpp"
#include <chrono>
#include <algorithm>

namespace arbitrage {

BinanceWebSocket::BinanceWebSocket(net::io_context& ioc, ssl::context& ctx)
    : WebSocketClientBase(ioc, ctx, Exchange::Binance) {
}

void BinanceWebSocket::subscribe_ticker(const std::vector<std::string>& symbols) {
    for (const auto& symbol : symbols) {
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(),
                      lower_symbol.begin(), ::tolower);
        streams_.add((lower_symbol + "@ticker").c_str());
    }
}

void BinanceWebSocket::subscribe_orderbook(const std::vector<std::string>& symbols, int depth) {
    orderbook_depth_ = depth;
    for (const auto& symbol : symbols) {
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(),
                      lower_symbol.begin(), ::tolower);
        streams_.add((lower_symbol + "@depth" + std::to_string(depth)).c_str());
    }
}

void BinanceWebSocket::subscribe_trade(const std::vector<std::string>& symbols) {
    for (const auto& symbol : symbols) {
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(),
                      lower_symbol.begin(), ::tolower);
        streams_.add((lower_symbol + "@trade").c_str());
    }
}

void BinanceWebSocket::connect_with_streams() {
    std::string target = "/stream";
    if (!streams_.empty()) {
        target += "?streams=";
        for (size_t i = 0; i < streams_.size(); ++i) {
            if (i > 0) target += "/";
            target += streams_.get(i);
        }
    }

    connect("stream.binance.com", "9443", target);
}

std::string BinanceWebSocket::build_subscribe_message() {
    // Combined Stream 사용 시 별도 구독 메시지 불필요
    if (!streams_.empty()) {
        return "";
    }

    // 개별 구독 메시지 (대안) — nlohmann/json으로 빌드
    nlohmann::json params = nlohmann::json::array();
    for (size_t i = 0; i < streams_.size(); ++i) {
        params.push_back(streams_.get(i));
    }

    nlohmann::json subscribe_msg = {
        {"method", "SUBSCRIBE"},
        {"params", params},
        {"id", subscribe_id_++}
    };

    return subscribe_msg.dump();
}

// =============================================================================
// simdjson SIMD 가속 파싱 (AVX2/SSE4.2)
// =============================================================================

void BinanceWebSocket::parse_message(const std::string& message) {
    auto& parser = thread_local_simd_parser();
    simdjson::dom::element doc;
    if (parser.parse(message).get(doc) != simdjson::SUCCESS) {
        logger_->error("[Binance] SIMD JSON parse error");
        return;
    }

    // Combined Stream 메시지 형식: {"stream": "...", "data": {...}}
    if (simd_has_field(doc, "stream") && simd_has_field(doc, "data")) {
        std::string_view stream = simd_get_sv(doc["stream"]);
        simdjson::dom::element data;
        if (doc["data"].get(data) != simdjson::SUCCESS) return;

        // 스트림 이름에서 심볼 추출 (예: "xrpusdt@depth10" -> "XRPUSDT")
        std::string symbol;
        auto at_pos = stream.find('@');
        if (at_pos != std::string_view::npos) {
            symbol = std::string(stream.substr(0, at_pos));
            std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
        }

        if (stream.find("@ticker") != std::string_view::npos) {
            parse_ticker(data);
        } else if (stream.find("@depth") != std::string_view::npos) {
            parse_orderbook(data, symbol);
        } else if (stream.find("@aggTrade") != std::string_view::npos ||
                   stream.find("@trade") != std::string_view::npos) {
            parse_trade(data);
        }
    }
    // 단일 스트림 메시지: {"e": "eventType", ...}
    else if (simd_has_field(doc, "e")) {
        std::string_view event_type = simd_get_sv(doc["e"]);

        if (event_type == "24hrTicker") {
            parse_ticker(doc);
        } else if (event_type == "depthUpdate") {
            parse_orderbook(doc, "");
        } else if (event_type == "trade" || event_type == "aggTrade") {
            parse_trade(doc);
        }
    }
}

void BinanceWebSocket::parse_ticker(simdjson::dom::element data) {
    Ticker ticker;
    ticker.exchange = Exchange::Binance;
    ticker.set_symbol(simd_get_sv(data["s"]));
    ticker.price = simd_get_double_or(data["c"]);   // Last price (문자열)
    ticker.bid = simd_get_double_or(data["b"]);      // Best bid
    ticker.ask = simd_get_double_or(data["a"]);      // Best ask
    ticker.volume_24h = simd_get_double_or(data["v"]); // Volume
    ticker.timestamp_us = simd_get_int64(data["E"]) * 1000;

    logger_->info("[Binance] Ticker - Symbol: {}, Price: {} USDT",
                  ticker.symbol, ticker.price);

    WebSocketEvent evt(WebSocketEvent::Type::Ticker, Exchange::Binance, ticker);
    emit_event(std::move(evt));
}

void BinanceWebSocket::parse_orderbook(simdjson::dom::element data, std::string_view stream_symbol) {
    OrderBook orderbook;
    orderbook.exchange = Exchange::Binance;
    orderbook.clear();

    // 심볼 추출
    auto sym = simd_get_sv(data["s"]);
    if (!sym.empty()) {
        orderbook.set_symbol(sym);
    } else if (!stream_symbol.empty()) {
        orderbook.set_symbol(stream_symbol);
    }

    // SIMD 가속 호가 파싱: Binance는 [["price", "qty"], ...] 형식
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

    // 타임스탬프 (밀리초 → 마이크로초)
    int64_t ts = simd_get_int64(data["E"]);
    if (ts > 0) {
        orderbook.timestamp_us = ts * 1000;
    } else {
        auto now = std::chrono::system_clock::now();
        orderbook.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
    }

    if (orderbook.bid_count > 0 && orderbook.ask_count > 0) {
        logger_->info("[Binance] OrderBook - Symbol: {}, Bids: {}, Asks: {}, BestBid: {}, BestAsk: {}",
                      orderbook.symbol, orderbook.bid_count, orderbook.ask_count,
                      orderbook.best_bid(), orderbook.best_ask());
    }

    WebSocketEvent evt(WebSocketEvent::Type::OrderBook, Exchange::Binance, orderbook);
    emit_event(std::move(evt));
}

void BinanceWebSocket::parse_trade(simdjson::dom::element data) {
    Ticker trade;
    trade.exchange = Exchange::Binance;
    trade.set_symbol(simd_get_sv(data["s"]));
    trade.price = simd_get_double_or(data["p"]);
    trade.bid = trade.price;
    trade.ask = trade.price;
    trade.volume_24h = simd_get_double_or(data["q"]);
    trade.timestamp_us = simd_get_int64(data["T"]) * 1000;

    logger_->info("[Binance] Trade - Symbol: {}, Price: {} USDT",
                  trade.symbol, trade.price);

    WebSocketEvent evt(WebSocketEvent::Type::Trade, Exchange::Binance, trade);
    emit_event(std::move(evt));
}

}  // namespace arbitrage
