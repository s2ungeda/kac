#include "arbitrage/exchange/mexc/websocket.hpp"
#include "arbitrage/common/json.hpp"
#include <chrono>
#include <algorithm>
#include <cctype>

namespace arbitrage {

MEXCWebSocket::MEXCWebSocket(net::io_context& ioc, ssl::context& ctx)
    : WebSocketClientBase(ioc, ctx, Exchange::MEXC) {
}

void MEXCWebSocket::subscribe_ticker(const std::vector<std::string>& symbols) {
    ticker_symbols_ = symbols;
}

void MEXCWebSocket::subscribe_orderbook(const std::vector<std::string>& symbols) {
    orderbook_symbols_ = symbols;
}

void MEXCWebSocket::subscribe_trade(const std::vector<std::string>& symbols) {
    trade_symbols_ = symbols;
}

void MEXCWebSocket::on_connected() {
    // MEXC는 연결 후 구독 메시지 전송
    logger_->info("[MEXC] Connected, starting subscriptions...");

    // 구독 상태 초기화
    ticker_idx_ = 0;
    orderbook_idx_ = 0;
    trade_idx_ = 0;
    ticker_done_ = ticker_symbols_.empty();
    orderbook_done_ = orderbook_symbols_.empty();
    trade_done_ = trade_symbols_.empty();

    // 구독 시작
    send_subscriptions();
}

std::string MEXCWebSocket::build_subscribe_message() {
    // MEXC는 on_connected에서 처리
    return "";
}

// 심볼 형식 변환: XRPUSDT -> XRP_USDT
std::string MEXCWebSocket::convert_to_futures_symbol(const std::string& symbol) {
    // 이미 언더스코어가 있으면 그대로 반환
    if (symbol.find('_') != std::string::npos) {
        return symbol;
    }

    // USDT 위치 찾기
    size_t usdt_pos = symbol.find("USDT");
    if (usdt_pos != std::string::npos && usdt_pos > 0) {
        return symbol.substr(0, usdt_pos) + "_USDT";
    }

    // USDC 위치 찾기
    size_t usdc_pos = symbol.find("USDC");
    if (usdc_pos != std::string::npos && usdc_pos > 0) {
        return symbol.substr(0, usdc_pos) + "_USDC";
    }

    return symbol;
}

void MEXCWebSocket::send_subscriptions() {
    // Ticker 구독
    for (const auto& symbol : ticker_symbols_) {
        std::string futures_symbol = convert_to_futures_symbol(symbol);
        nlohmann::json sub_msg = {
            {"method", "sub.ticker"},
            {"param", {{"symbol", futures_symbol}}}
        };
        logger_->info("[MEXC] Subscribing to ticker: {}", futures_symbol);
        send_message(sub_msg.dump());
    }

    // Deal(Trade) 구독
    for (const auto& symbol : trade_symbols_) {
        std::string futures_symbol = convert_to_futures_symbol(symbol);
        nlohmann::json sub_msg = {
            {"method", "sub.deal"},
            {"param", {{"symbol", futures_symbol}}}
        };
        logger_->info("[MEXC] Subscribing to deals: {}", futures_symbol);
        send_message(sub_msg.dump());
    }

    // Orderbook(Depth) 구독
    for (const auto& symbol : orderbook_symbols_) {
        std::string futures_symbol = convert_to_futures_symbol(symbol);
        nlohmann::json sub_msg = {
            {"method", "sub.depth"},
            {"param", {{"symbol", futures_symbol}}}
        };
        logger_->info("[MEXC] Subscribing to depth: {}", futures_symbol);
        send_message(sub_msg.dump());
    }
}

void MEXCWebSocket::parse_message(const std::string& message) {
    try {
        if (message.empty()) {
            return;
        }

        auto json = nlohmann::json::parse(message);

        // Pong 응답 처리 (다양한 형식 지원)
        if (json.contains("channel") && json["channel"] == "pong") {
            logger_->debug("[MEXC] Received pong (channel)");
            return;
        }
        if (json.contains("data") && json["data"] == "pong") {
            logger_->debug("[MEXC] Received pong (data)");
            return;
        }

        // 구독 응답 처리
        if (json.contains("channel") && json["channel"] == "rs.sub.ticker") {
            logger_->info("[MEXC] Ticker subscription confirmed");
            return;
        }
        if (json.contains("channel") && json["channel"] == "rs.sub.deal") {
            logger_->info("[MEXC] Deal subscription confirmed");
            return;
        }
        if (json.contains("channel") && json["channel"] == "rs.sub.depth") {
            logger_->info("[MEXC] Depth subscription confirmed");
            return;
        }

        // 데이터 메시지 처리
        if (json.contains("channel")) {
            std::string channel = json["channel"];

            if (channel == "push.ticker") {
                parse_ticker(json);
            } else if (channel == "push.deal") {
                parse_deal(json);
            } else if (channel == "push.depth") {
                parse_depth(json);
            }
        }

    } catch (const std::exception& e) {
        logger_->error("[MEXC] Parse error: {}", e.what());
    }
}

void MEXCWebSocket::parse_ticker(const nlohmann::json& json) {
    try {
        if (!json.contains("data")) return;

        auto data = json["data"];

        Ticker ticker;
        ticker.exchange = Exchange::MEXC;

        // 심볼
        if (data.contains("symbol")) {
            ticker.set_symbol(data["symbol"].get<std::string>());
        }

        // 최종 체결가
        if (data.contains("lastPrice")) {
            ticker.price = data["lastPrice"].get<double>();
        }

        // 매수/매도 호가
        if (data.contains("bid1")) {
            ticker.bid = data["bid1"].get<double>();
        } else {
            ticker.bid = ticker.price;
        }
        if (data.contains("ask1")) {
            ticker.ask = data["ask1"].get<double>();
        } else {
            ticker.ask = ticker.price;
        }

        // 24시간 거래량
        if (data.contains("volume24")) {
            ticker.volume_24h = data["volume24"].get<double>();
        }

        // 타임스탬프 (마이크로초로 변환)
        if (data.contains("timestamp")) {
            int64_t ts = data["timestamp"].get<int64_t>();
            ticker.timestamp_us = ts * 1000;
        } else {
            ticker.set_timestamp_now();
        }

        logger_->info("[MEXC] Ticker - Symbol: {}, Price: {}, Bid: {}, Ask: {}",
                      ticker.symbol, ticker.price, ticker.bid, ticker.ask);

        WebSocketEvent evt(WebSocketEvent::Type::Ticker, Exchange::MEXC, ticker);
        emit_event(std::move(evt));

    } catch (const std::exception& e) {
        logger_->error("[MEXC] Error parsing ticker: {}", e.what());
    }
}

void MEXCWebSocket::parse_deal(const nlohmann::json& json) {
    try {
        if (!json.contains("data")) return;

        auto data = json["data"];

        // data가 배열인 경우
        if (data.is_array()) {
            for (const auto& deal : data) {
                process_single_deal(deal, json.value("symbol", ""));
            }
        } else {
            // 단일 객체인 경우
            process_single_deal(data, json.value("symbol", ""));
        }

    } catch (const std::exception& e) {
        logger_->error("[MEXC] Error parsing deal: {}", e.what());
    }
}

void MEXCWebSocket::process_single_deal(const nlohmann::json& deal, const std::string& default_symbol) {
    Ticker trade;
    trade.exchange = Exchange::MEXC;

    // 심볼
    if (deal.contains("symbol")) {
        trade.set_symbol(deal["symbol"].get<std::string>());
    } else {
        trade.set_symbol(default_symbol);
    }

    // 체결가
    if (deal.contains("p")) {
        trade.price = deal["p"].get<double>();
    } else if (deal.contains("price")) {
        trade.price = deal["price"].get<double>();
    }

    trade.bid = trade.price;
    trade.ask = trade.price;

    // 체결량
    if (deal.contains("v")) {
        trade.volume_24h = deal["v"].get<double>();
    } else if (deal.contains("vol")) {
        trade.volume_24h = deal["vol"].get<double>();
    }

    // 타임스탬프 (마이크로초로 변환)
    if (deal.contains("t")) {
        int64_t ts = deal["t"].get<int64_t>();
        trade.timestamp_us = ts * 1000;
    } else if (deal.contains("ts")) {
        int64_t ts = deal["ts"].get<int64_t>();
        trade.timestamp_us = ts * 1000;
    } else {
        trade.set_timestamp_now();
    }

    logger_->info("[MEXC] Deal - Symbol: {}, Price: {}, Vol: {}",
                  trade.symbol, trade.price, trade.volume_24h);

    // Trade 이벤트
    WebSocketEvent trade_evt(WebSocketEvent::Type::Trade, Exchange::MEXC, trade);
    emit_event(std::move(trade_evt));

    // Ticker로도 emit (premium calculator용)
    Ticker ticker = trade;
    WebSocketEvent ticker_evt(WebSocketEvent::Type::Ticker, Exchange::MEXC, ticker);
    emit_event(std::move(ticker_evt));
}

void MEXCWebSocket::parse_depth(const nlohmann::json& json) {
    try {
        if (!json.contains("data")) return;

        auto data = json["data"];

        OrderBook orderbook;
        orderbook.exchange = Exchange::MEXC;
        orderbook.clear();

        // 심볼
        if (json.contains("symbol")) {
            orderbook.set_symbol(json["symbol"].get<std::string>());
        }

        // Asks (매도 호가)
        if (data.contains("asks")) {
            for (const auto& ask : data["asks"]) {
                orderbook.add_ask(ask[0].get<double>(), ask[1].get<double>());
            }
        }

        // Bids (매수 호가)
        if (data.contains("bids")) {
            for (const auto& bid : data["bids"]) {
                orderbook.add_bid(bid[0].get<double>(), bid[1].get<double>());
            }
        }

        auto now = std::chrono::system_clock::now();
        orderbook.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();

        if (orderbook.bid_count > 0 && orderbook.ask_count > 0) {
            logger_->debug("[MEXC] Depth - Best Bid: {}, Best Ask: {}",
                          orderbook.bids[0].price, orderbook.asks[0].price);
        }

        WebSocketEvent evt(WebSocketEvent::Type::OrderBook, Exchange::MEXC, orderbook);
        emit_event(std::move(evt));

    } catch (const std::exception& e) {
        logger_->error("[MEXC] Error parsing depth: {}", e.what());
    }
}

std::chrono::seconds MEXCWebSocket::ping_interval() const {
    // MEXC Futures는 10초 간격 권장
    return std::chrono::seconds(10);
}

}  // namespace arbitrage
