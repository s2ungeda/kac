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
    // MEXC는 연결 후 즉시 첫 구독 전송
    logger_->info("[MEXC] Connected, starting subscriptions...");
    send_delayed_subscriptions();
}

std::string MEXCWebSocket::build_subscribe_message() {
    // MEXC는 on_connected에서 처리하므로 여기서는 빈 문자열 반환
    return "";
}

void MEXCWebSocket::send_delayed_subscriptions() {
    // 하나의 구독만 전송
    nlohmann::json sub_msg;
    
    static size_t ticker_idx = 0;
    static size_t trade_idx = 0;
    static bool ticker_done = false;
    static bool trade_done = false;
    
    if (!ticker_done && ticker_idx < ticker_symbols_.size()) {
        // MEXC는 소문자 심볼을 요구할 수 있음
        std::string symbol = ticker_symbols_[ticker_idx];
        std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::tolower);
        
        sub_msg = {
            {"method", "SUBSCRIPTION"},
            {"params", nlohmann::json::array({
                "spot@public.ticker.v3.api@" + symbol
            })},
            {"id", subscribe_id_++}
        };
        ticker_idx++;
        if (ticker_idx >= ticker_symbols_.size()) ticker_done = true;
    } else if (!trade_done && trade_idx < trade_symbols_.size()) {
        // MEXC는 소문자 심볼을 요구할 수 있음
        std::string symbol = trade_symbols_[trade_idx];
        std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::tolower);
        
        sub_msg = {
            {"method", "SUBSCRIPTION"},
            {"params", nlohmann::json::array({
                "spot@public.deals.v3.api@" + symbol
            })},
            {"id", subscribe_id_++}
        };
        trade_idx++;
        if (trade_idx >= trade_symbols_.size()) trade_done = true;
    }
    
    if (!sub_msg.empty()) {
        logger_->info("[MEXC] Sending subscription for channel");
        send_message(sub_msg.dump());
    }
}

void MEXCWebSocket::parse_message(const std::string& message) {
    try {
        auto json = nlohmann::json::parse(message);
        
        // 모든 메시지 디버깅 - 제거됨
        
        // 구독 응답 처리
        if (json.contains("code")) {
            if (json.contains("msg")) {
                std::string msg = json["msg"];
                if (msg.find("Not Subscribed successfully") != std::string::npos) {
                    logger_->warn("[MEXC] Subscription failed: {}", msg);
                    // 실패해도 다음 구독 시도
                    send_delayed_subscriptions();
                } else {
                    logger_->info("[MEXC] Subscription response: {}", msg);
                    // 성공 시 다음 구독 전송
                    send_delayed_subscriptions();
                }
            }
            return;
        }
        
        // Ping 응답 처리
        if (json.contains("ping")) {
            send_message("{\"pong\":" + std::to_string(json["ping"].get<int64_t>()) + "}");
            // Pong sent
            return;
        }
        
        // 데이터 메시지 처리
        if (json.contains("d") && json.contains("c")) {
            std::string channel = json["c"];
            auto data = json["d"];
            
            // Data channel received
            
            if (channel.find("ticker") != std::string::npos) {
                parse_ticker(data);
            } else if (channel.find("depth") != std::string::npos) {
                parse_orderbook(data);
            } else if (channel.find("deals") != std::string::npos) {
                parse_trade(data);
            }
        }
        
    } catch (const std::exception& e) {
        logger_->error("[MEXC] Parse error: {}", e.what());
    }
}

void MEXCWebSocket::parse_ticker(const nlohmann::json& data) {
    Ticker ticker;
    ticker.exchange = Exchange::MEXC;
    
    // ticker 데이터 형식
    try {
        // 심볼
        if (data.contains("s")) {
            ticker.symbol = data["s"];
        }
        
        // 현재가 (최종 체결가)
        if (data.contains("c")) {
            ticker.price = std::stod(data["c"].get<std::string>());
        }
        
        // 매수/매도 호가
        if (data.contains("b")) {
            ticker.bid = std::stod(data["b"].get<std::string>());  // Best bid price
        }
        if (data.contains("a")) {
            ticker.ask = std::stod(data["a"].get<std::string>());  // Best ask price
        }
        
        // 24시간 거래량
        if (data.contains("v")) {
            ticker.volume_24h = std::stod(data["v"].get<std::string>());
        }
        
        // 타임스탬프
        if (data.contains("t")) {
            int64_t timestamp_ms = data["t"];
            ticker.timestamp = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(timestamp_ms)
            );
        } else {
            ticker.timestamp = std::chrono::system_clock::now();
        }
        
        logger_->info("[MEXC] Ticker parsed - Symbol: {}, Price: {}, Bid: {}, Ask: {}", 
                     ticker.symbol, ticker.price, ticker.bid, ticker.ask);
        
        WebSocketEvent evt(WebSocketEvent::Type::Ticker, Exchange::MEXC, ticker);
        
        emit_event(std::move(evt));
    } catch (const std::exception& e) {
        logger_->error("[MEXC] Error parsing ticker: {}", e.what());
    }
}

void MEXCWebSocket::parse_orderbook(const nlohmann::json& data) {
    OrderBook orderbook;
    orderbook.exchange = Exchange::MEXC;
    
    // 심볼 추출
    if (data.contains("s")) {
        orderbook.symbol = data["s"];
    }
    
    // Bids (매수 호가)
    if (data.contains("bids")) {
        for (const auto& bid : data["bids"]) {
            PriceLevel level;
            level.price = std::stod(bid["p"].get<std::string>());
            level.quantity = std::stod(bid["v"].get<std::string>());
            orderbook.bids.push_back(level);
        }
    }
    
    // Asks (매도 호가)
    if (data.contains("asks")) {
        for (const auto& ask : data["asks"]) {
            PriceLevel level;
            level.price = std::stod(ask["p"].get<std::string>());
            level.quantity = std::stod(ask["v"].get<std::string>());
            orderbook.asks.push_back(level);
        }
    }
    
    // 타임스탬프
    if (data.contains("t")) {
        int64_t timestamp_ms = data["t"];
        orderbook.timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(timestamp_ms)
        );
    } else {
        orderbook.timestamp = std::chrono::system_clock::now();
    }
    
    // OrderBook 파싱 결과 출력
    if (!orderbook.bids.empty() && !orderbook.asks.empty()) {
        logger_->info("[MEXC] OrderBook parsed - Symbol: {}, Best Bid: {}, Best Ask: {}",
                     orderbook.symbol, orderbook.bids[0].price, orderbook.asks[0].price);
    }
    
    WebSocketEvent evt(WebSocketEvent::Type::OrderBook, Exchange::MEXC, orderbook);
    
    emit_event(std::move(evt));
}

void MEXCWebSocket::parse_trade(const nlohmann::json& data) {
    // MEXC는 deals 데이터를 배열로 제공
    if (data.contains("deals") && data["deals"].is_array() && !data["deals"].empty()) {
        for (const auto& deal : data["deals"]) {
            Ticker trade;
            trade.exchange = Exchange::MEXC;
            
            // 심볼
            if (data.contains("symbol")) {
                trade.symbol = data["symbol"];
            }
            
            // 체결가
            trade.price = std::stod(deal["p"].get<std::string>());
            
            // MEXC trade에는 bid/ask 정보가 없으므로 체결가로 설정
            trade.bid = trade.price;
            trade.ask = trade.price;
            
            // 체결량
            trade.volume_24h = std::stod(deal["v"].get<std::string>());
            
            // 타임스탬프
            if (deal.contains("t")) {
                int64_t timestamp_ms = deal["t"];
                trade.timestamp = std::chrono::system_clock::time_point(
                    std::chrono::milliseconds(timestamp_ms)
                );
            } else {
                trade.timestamp = std::chrono::system_clock::now();
            }
            
            logger_->info("[MEXC] Trade parsed - Symbol: {}, Price: {}, Volume: {}",
                         trade.symbol, trade.price, trade.volume_24h);
            
            WebSocketEvent evt(WebSocketEvent::Type::Trade, Exchange::MEXC, trade);
            emit_event(std::move(evt));
        }
    }
}

}  // namespace arbitrage