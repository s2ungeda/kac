#include "arbitrage/exchange/mexc/websocket.hpp"
#include "arbitrage/common/json.hpp"
#include <chrono>

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

std::string MEXCWebSocket::build_subscribe_message() {
    nlohmann::json messages = nlohmann::json::array();
    
    // 시세 구독 (deals - 체결 정보로 대체)
    for (const auto& symbol : ticker_symbols_) {
        messages.push_back({
            {"method", "SUBSCRIPTION"},
            {"params", nlohmann::json::array({
                "spot@public.deals.v3.api@" + symbol
            })},
            {"id", subscribe_id_++}
        });
    }
    
    // 호가 구독
    for (const auto& symbol : orderbook_symbols_) {
        messages.push_back({
            {"method", "SUBSCRIPTION"},
            {"params", nlohmann::json::array({
                "spot@public.limit.depth.v3.api@" + symbol + "@20"
            })},
            {"id", subscribe_id_++}
        });
    }
    
    // 체결 구독
    for (const auto& symbol : trade_symbols_) {
        messages.push_back({
            {"method", "SUBSCRIPTION"},
            {"params", nlohmann::json::array({
                "spot@public.deals.v3.api@" + symbol
            })},
            {"id", subscribe_id_++}
        });
    }
    
    // MEXC는 각 구독을 별도로 전송
    if (!messages.empty()) {
        return messages[0].dump();  // 첫 번째만 반환, 나머지는 연결 후 전송
    }
    
    return "";
}

void MEXCWebSocket::parse_message(const std::string& message) {
    try {
        auto json = nlohmann::json::parse(message);
        
        // 모든 메시지 디버깅
        logger_->debug("[MEXC] Received message: {}", json.dump());
        
        // 구독 응답 처리
        if (json.contains("code") && json["code"] == 0) {
            logger_->debug("[MEXC] Subscription successful");
            
            // 나머지 구독 메시지 전송
            nlohmann::json messages = nlohmann::json::array();
            
            // 위에서 첫 번째만 전송했으므로 나머지 전송
            for (size_t i = 1; i < ticker_symbols_.size(); ++i) {
                send_message(nlohmann::json({
                    {"method", "SUBSCRIPTION"},
                    {"params", nlohmann::json::array({
                        "spot@public.deals.v3.api@" + ticker_symbols_[i]
                    })},
                    {"id", subscribe_id_++}
                }).dump());
            }
            
            for (const auto& symbol : orderbook_symbols_) {
                send_message(nlohmann::json({
                    {"method", "SUBSCRIPTION"},
                    {"params", nlohmann::json::array({
                        "spot@public.limit.depth.v3.api@" + symbol + "@20"
                    })},
                    {"id", subscribe_id_++}
                }).dump());
            }
            
            return;
        }
        
        // Ping 응답 처리
        if (json.contains("ping")) {
            send_message("{\"pong\":" + std::to_string(json["ping"].get<int64_t>()) + "}");
            logger_->debug("[MEXC] Sent pong response");
            return;
        }
        
        // 데이터 메시지 처리
        if (json.contains("d") && json.contains("c")) {
            std::string channel = json["c"];
            auto data = json["d"];
            
            logger_->debug("[MEXC] Data channel: {}", channel);
            
            if (channel.find("deals") != std::string::npos) {
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
    
    // deals 데이터의 경우, 배열로 옴
    if (data.contains("deals") && data["deals"].is_array() && !data["deals"].empty()) {
        auto& deal = data["deals"][0];  // 가장 최근 체결
        ticker.symbol = data["symbol"];  
        ticker.price = std::stod(deal["p"].get<std::string>());  // 체결가
        
        // bid/ask는 체결 데이터에서 얻을 수 없으므로 price로 대체
        ticker.bid = ticker.price;
        ticker.ask = ticker.price;
        
        ticker.volume_24h = 0;  // deals에서는 24시간 거래량 제공 안함
        
        // 타임스탬프
        if (deal.contains("t")) {
            int64_t timestamp_ms = deal["t"];
            ticker.timestamp = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(timestamp_ms)
            );
        } else {
            ticker.timestamp = std::chrono::system_clock::now();
        }
        
        WebSocketEvent evt{
            WebSocketEvent::Type::Ticker,
            Exchange::MEXC,
            ticker,
            ""
        };
        
        emit_event(std::move(evt));
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
    
    WebSocketEvent evt{
        WebSocketEvent::Type::OrderBook,
        Exchange::MEXC,
        orderbook,
        ""
    };
    
    emit_event(std::move(evt));
}

void MEXCWebSocket::parse_trade(const nlohmann::json& data) {
    // Trade 이벤트는 현재 사용하지 않으므로 구현 생략
}

}  // namespace arbitrage