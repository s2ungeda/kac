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
// 필드 매핑 + simdjson SIMD 가속 파싱
// =============================================================================

namespace {
    const TickerFieldMap BITHUMB_TICKER_MAP {
        "code", "trade_price", "best_bid_price", "best_ask_price",
        "acc_trade_volume_24h", "",  // timestamp empty = use current time
        0, true  // bid_ask_fallback=true
    };
    const OrderBookFieldMap BITHUMB_OB_MAP {
        "code", "", 0,  // no timestamp from API
        OrderBookFieldMap::OBJECTS,
        "orderbook_units", "ask_price", "ask_size", "bid_price", "bid_size", true,
        "", ""
    };
    const TradeFieldMap BITHUMB_TRADE_MAP {
        "code", "trade_price", "trade_volume", "trade_timestamp", 1000
    };
}

void BithumbWebSocket::parse_message(const std::string& message) {
    auto& parser = thread_local_simd_parser();
    simdjson::dom::element doc;
    if (parser.parse(message).get(doc) != simdjson::SUCCESS) {
        logger_->error("[Bithumb] SIMD JSON parse error");
        return;
    }

    if (simd_has_field(doc, "error")) {
        logger_->error("[Bithumb] Error response");
        return;
    }

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
    // SNAPSHOT 무시 (REALTIME만)
    std::string_view stream_type = simd_get_sv(json["stream_type"]);
    if (stream_type == "SNAPSHOT") return;

    auto trade = make_trade(json, BITHUMB_TRADE_MAP);
    if (trade.price <= 0.0) return;

    logger_->info("[Bithumb] Trade - Code: {}, Price: {} KRW, Vol: {}",
                  trade.symbol, trade.price, trade.volume_24h);

    emit_event({WebSocketEvent::Type::Trade, Exchange::Bithumb, trade});

    // Ticker로도 emit (premium calculator용)
    Ticker ticker = trade;
    emit_event({WebSocketEvent::Type::Ticker, Exchange::Bithumb, ticker});
}

void BithumbWebSocket::parse_ticker_v2(simdjson::dom::element json) {
    auto ticker = make_ticker(json, BITHUMB_TICKER_MAP);
    logger_->info("[Bithumb] Ticker - Code: {}, Price: {} KRW",
                  ticker.symbol, ticker.price);
    emit_event({WebSocketEvent::Type::Ticker, Exchange::Bithumb, ticker});
}

void BithumbWebSocket::parse_orderbook_v2(simdjson::dom::element json) {
    auto orderbook = make_orderbook(json, BITHUMB_OB_MAP);

    if (orderbook.bid_count > 0 && orderbook.ask_count > 0) {
        logger_->info("[Bithumb] OrderBook - Best Bid: {}, Best Ask: {}",
                      orderbook.bids[0].price, orderbook.asks[0].price);
    }
    emit_event({WebSocketEvent::Type::OrderBook, Exchange::Bithumb, orderbook});
}

}  // namespace arbitrage
