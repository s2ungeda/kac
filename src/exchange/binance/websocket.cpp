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
// 필드 매핑 + simdjson SIMD 가속 파싱
// =============================================================================

namespace {
    const TickerFieldMap BINANCE_TICKER_MAP {
        "s", "c", "b", "a", "v", "E", 1000, false
    };
    const OrderBookFieldMap BINANCE_OB_MAP {
        "s", "E", 1000,
        OrderBookFieldMap::TUPLES,
        "", "", "", "", "", false,
        "bids", "asks"
    };
    const TradeFieldMap BINANCE_TRADE_MAP {
        "s", "p", "q", "T", 1000
    };
}

void BinanceWebSocket::parse_message(const std::string& message) {
    auto& parser = thread_local_simd_parser();
    simdjson::dom::element doc;
    if (parser.parse(message).get(doc) != simdjson::SUCCESS) {
        logger_->error("[Binance] SIMD JSON parse error");
        return;
    }

    // Combined Stream: {"stream": "...", "data": {...}}
    if (simd_has_field(doc, "stream") && simd_has_field(doc, "data")) {
        std::string_view stream = simd_get_sv(doc["stream"]);
        simdjson::dom::element data;
        if (doc["data"].get(data) != simdjson::SUCCESS) return;

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
    // 단일 스트림: {"e": "eventType", ...}
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
    auto ticker = make_ticker(data, BINANCE_TICKER_MAP);
    logger_->info("[Binance] Ticker - Symbol: {}, Price: {} USDT",
                  ticker.symbol, ticker.price);
    emit_event({WebSocketEvent::Type::Ticker, Exchange::Binance, ticker});
}

void BinanceWebSocket::parse_orderbook(simdjson::dom::element data, std::string_view stream_symbol) {
    auto orderbook = make_orderbook(data, BINANCE_OB_MAP, stream_symbol);

    if (orderbook.bid_count > 0 && orderbook.ask_count > 0) {
        logger_->info("[Binance] OrderBook - Symbol: {}, Bids: {}, Asks: {}, BestBid: {}, BestAsk: {}",
                      orderbook.symbol, orderbook.bid_count, orderbook.ask_count,
                      orderbook.best_bid(), orderbook.best_ask());
    }
    emit_event({WebSocketEvent::Type::OrderBook, Exchange::Binance, orderbook});
}

void BinanceWebSocket::parse_trade(simdjson::dom::element data) {
    auto trade = make_trade(data, BINANCE_TRADE_MAP);
    logger_->info("[Binance] Trade - Symbol: {}, Price: {} USDT",
                  trade.symbol, trade.price);
    emit_event({WebSocketEvent::Type::Trade, Exchange::Binance, trade});
}

}  // namespace arbitrage
