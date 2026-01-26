#include "arbitrage/exchange/bithumb/websocket.hpp"
#include "arbitrage/common/json.hpp"
#include <chrono>
#include <map>
#include <algorithm>

namespace arbitrage {

BithumbWebSocket::BithumbWebSocket(net::io_context& ioc, ssl::context& ctx)
    : WebSocketClientBase(ioc, ctx, Exchange::Bithumb) {
}

void BithumbWebSocket::subscribe_ticker(const std::vector<std::string>& symbols) {
    // 심볼 형식 변환: XRP_KRW -> KRW-XRP
    for (const auto& sym : symbols) {
        ticker_codes_.push_back(convert_to_v2_code(sym));
    }
}

void BithumbWebSocket::subscribe_orderbook(const std::vector<std::string>& symbols) {
    for (const auto& sym : symbols) {
        orderbook_codes_.push_back(convert_to_v2_code(sym));
    }
}

void BithumbWebSocket::subscribe_trade(const std::vector<std::string>& symbols) {
    for (const auto& sym : symbols) {
        trade_codes_.push_back(convert_to_v2_code(sym));
    }
}

// 심볼 변환: XRP_KRW -> KRW-XRP
std::string BithumbWebSocket::convert_to_v2_code(const std::string& symbol) {
    // 이미 KRW- 형식이면 그대로 반환
    if (symbol.find("KRW-") == 0) {
        return symbol;
    }

    // XRP_KRW -> KRW-XRP
    size_t pos = symbol.find("_KRW");
    if (pos != std::string::npos) {
        return "KRW-" + symbol.substr(0, pos);
    }

    // XRPKRW -> KRW-XRP
    pos = symbol.find("KRW");
    if (pos != std::string::npos && pos > 0) {
        return "KRW-" + symbol.substr(0, pos);
    }

    return symbol;
}

std::string BithumbWebSocket::build_subscribe_message() {
    // 빗썸 v2 API 형식: [ticket, type, format]
    nlohmann::json msg = nlohmann::json::array();

    // Ticket field
    msg.push_back({{"ticket", "arbitrage-cpp"}});

    // Type field - trade 우선
    if (!trade_codes_.empty()) {
        msg.push_back({
            {"type", "trade"},
            {"codes", trade_codes_},
            {"isOnlyRealtime", true}
        });
    } else if (!ticker_codes_.empty()) {
        msg.push_back({
            {"type", "ticker"},
            {"codes", ticker_codes_},
            {"isOnlyRealtime", true}
        });
    } else if (!orderbook_codes_.empty()) {
        msg.push_back({
            {"type", "orderbook"},
            {"codes", orderbook_codes_},
            {"isOnlyRealtime", true}
        });
    }

    // Format field
    msg.push_back({{"format", "DEFAULT"}});

    return msg.dump();
}

void BithumbWebSocket::parse_message(const std::string& message) {
    try {
        auto json = nlohmann::json::parse(message);

        // 에러 응답 처리
        if (json.contains("error")) {
            logger_->error("[Bithumb] Error: {}", json.dump());
            return;
        }

        // type 필드 확인
        if (!json.contains("type")) {
            // status 응답 (연결 확인 등)
            if (json.contains("status") && json["status"] == "UP") {
                logger_->info("[Bithumb] Connection status: UP");
            }
            return;
        }

        std::string type = json["type"];

        if (type == "trade") {
            parse_trade_v2(json);
        } else if (type == "ticker") {
            parse_ticker_v2(json);
        } else if (type == "orderbook") {
            parse_orderbook_v2(json);
        }

    } catch (const std::exception& e) {
        logger_->error("[Bithumb] Parse error: {}", e.what());
    }
}

void BithumbWebSocket::parse_trade_v2(const nlohmann::json& json) {
    Ticker trade;
    trade.exchange = Exchange::Bithumb;

    // code: "KRW-XRP" -> symbol 저장
    if (json.contains("code")) {
        trade.set_symbol(json["code"].get<std::string>());
    }

    // trade_price: 체결가
    if (json.contains("trade_price")) {
        trade.price = json["trade_price"].get<double>();
        trade.bid = trade.price;
        trade.ask = trade.price;
    }

    // trade_volume: 체결량
    if (json.contains("trade_volume")) {
        trade.volume_24h = json["trade_volume"].get<double>();
    }

    // trade_timestamp: 타임스탬프 (밀리초 -> 마이크로초)
    if (json.contains("trade_timestamp")) {
        int64_t ts = json["trade_timestamp"].get<int64_t>();
        trade.timestamp_us = ts * 1000;
    } else {
        auto now = std::chrono::system_clock::now();
        trade.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
    }

    // stream_type 확인 (REALTIME만 처리)
    if (json.contains("stream_type")) {
        std::string stream_type = json["stream_type"];
        if (stream_type == "SNAPSHOT") {
            return;  // 스냅샷은 무시
        }
    }

    logger_->info("[Bithumb] Trade - Code: {}, Price: {} KRW, Vol: {}",
                  trade.symbol, trade.price, trade.volume_24h);

    // Trade 이벤트
    WebSocketEvent trade_evt(WebSocketEvent::Type::Trade, Exchange::Bithumb, trade);
    emit_event(std::move(trade_evt));

    // Ticker로도 emit (premium calculator용)
    Ticker ticker = trade;
    WebSocketEvent ticker_evt(WebSocketEvent::Type::Ticker, Exchange::Bithumb, ticker);
    emit_event(std::move(ticker_evt));
}

void BithumbWebSocket::parse_ticker_v2(const nlohmann::json& json) {
    Ticker ticker;
    ticker.exchange = Exchange::Bithumb;

    if (json.contains("code")) {
        ticker.set_symbol(json["code"].get<std::string>());
    }

    // trade_price: 현재가
    if (json.contains("trade_price")) {
        ticker.price = json["trade_price"].get<double>();
    }

    // best_bid_price, best_ask_price
    if (json.contains("best_bid_price")) {
        ticker.bid = json["best_bid_price"].get<double>();
    } else {
        ticker.bid = ticker.price;
    }
    if (json.contains("best_ask_price")) {
        ticker.ask = json["best_ask_price"].get<double>();
    } else {
        ticker.ask = ticker.price;
    }

    // acc_trade_volume_24h: 24시간 거래량
    if (json.contains("acc_trade_volume_24h")) {
        ticker.volume_24h = json["acc_trade_volume_24h"].get<double>();
    }

    ticker.set_timestamp_now();

    logger_->info("[Bithumb] Ticker - Code: {}, Price: {} KRW",
                  ticker.symbol, ticker.price);

    WebSocketEvent evt(WebSocketEvent::Type::Ticker, Exchange::Bithumb, ticker);
    emit_event(std::move(evt));
}

void BithumbWebSocket::parse_orderbook_v2(const nlohmann::json& json) {
    OrderBook orderbook;
    orderbook.exchange = Exchange::Bithumb;
    orderbook.clear();

    if (json.contains("code")) {
        orderbook.set_symbol(json["code"].get<std::string>());
    }

    // orderbook_units: [{ask_price, bid_price, ask_size, bid_size}, ...]
    if (json.contains("orderbook_units") && json["orderbook_units"].is_array()) {
        for (const auto& unit : json["orderbook_units"]) {
            // Ask
            if (unit.contains("ask_price") && unit.contains("ask_size")) {
                orderbook.add_ask(
                    unit["ask_price"].get<double>(),
                    unit["ask_size"].get<double>()
                );
            }
            // Bid
            if (unit.contains("bid_price") && unit.contains("bid_size")) {
                orderbook.add_bid(
                    unit["bid_price"].get<double>(),
                    unit["bid_size"].get<double>()
                );
            }
        }
    }

    auto now = std::chrono::system_clock::now();
    orderbook.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    if (orderbook.bid_count > 0 && orderbook.ask_count > 0) {
        logger_->debug("[Bithumb] OrderBook - Best Bid: {}, Best Ask: {}",
                      orderbook.bids[0].price, orderbook.asks[0].price);
    }

    WebSocketEvent evt(WebSocketEvent::Type::OrderBook, Exchange::Bithumb, orderbook);
    emit_event(std::move(evt));
}

}  // namespace arbitrage
