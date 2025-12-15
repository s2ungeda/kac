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
    ticker_symbols_ = symbols;
}

void BithumbWebSocket::subscribe_orderbook(const std::vector<std::string>& symbols) {
    orderbook_symbols_ = symbols;
}

void BithumbWebSocket::subscribe_trade(const std::vector<std::string>& symbols) {
    trade_symbols_ = symbols;
}

std::string BithumbWebSocket::build_subscribe_message() {
    // 빗썸은 각 구독을 개별적으로 보내야 함
    // 첫 번째로 ticker 구독 메시지를 반환
    if (!ticker_symbols_.empty()) {
        nlohmann::json msg = {
            {"type", "ticker"},
            {"symbols", ticker_symbols_},
            {"tickTypes", nlohmann::json::array({"24H"})}
        };
        return msg.dump();
    }
    
    // ticker가 없으면 orderbook 구독
    if (!orderbook_symbols_.empty()) {
        nlohmann::json msg = {
            {"type", "orderbookdepth"},
            {"symbols", orderbook_symbols_}
        };
        return msg.dump();
    }
    
    // 둘 다 없으면 transaction 구독
    if (!trade_symbols_.empty()) {
        nlohmann::json msg = {
            {"type", "transaction"},
            {"symbols", trade_symbols_}
        };
        return msg.dump();
    }
    
    return "";
}

void BithumbWebSocket::parse_message(const std::string& message) {
    try {
        auto json = nlohmann::json::parse(message);
        
        // 디버깅을 위해 메시지 출력 - 제거됨
        
        // 연결 성공 메시지 처리
        if (json.contains("status") && json["status"] == "0000") {
            if (json.contains("resmsg")) {
                std::string msg = json["resmsg"];
                if (msg == "Connected Successfully") {
                    // 연결 성공 - 이미 첫 구독은 보냈으므로 추가 구독 전송
                    send_additional_subscriptions();
                    return;
                } else if (msg == "Filter Registered Successfully") {
                    // 구독 성공
                    logger_->info("[Bithumb] Filter registered successfully");
                    return;
                }
            }
        }
        
        // Bithumb는 type과 content 필드를 사용
        if (!json.contains("type")) {
            logger_->warn("[Bithumb] Message has no type field");
            return;
        }
        
        std::string type = json["type"];
        
        if (type == "ticker") {
            parse_ticker(json);
        } else if (type == "orderbookdepth") {
            parse_orderbook(json);
        } else if (type == "transaction") {
            parse_trade(json);
        } else {
            logger_->debug("[Bithumb] Unknown message type: {}", type);
        }
        
    } catch (const std::exception& e) {
        logger_->error("[Bithumb] Parse error: {}", e.what());
    }
}

void BithumbWebSocket::parse_ticker(const nlohmann::json& data) {
    if (!data.contains("content")) {
        return;
    }
    
    auto content = data["content"];
    
    // content가 배열인 경우 처리
    if (content.is_array()) {
        for (const auto& item : content) {
            parse_single_ticker(item);
        }
        return;
    }
    
    // content가 객체인 경우 처리
    parse_single_ticker(content);
}

void BithumbWebSocket::parse_single_ticker(const nlohmann::json& content) {
    // 실제 데이터 구조 확인을 위한 디버깅 - 제거됨
    
    // 필수 필드 체크 - 빗썸 ticker는 호가 정보 없음
    if (!content.contains("symbol") || !content.contains("closePrice")) {
        logger_->warn("[Bithumb] Missing required fields in ticker data: {}", content.dump());
        return;
    }
    Ticker ticker;
    ticker.exchange = Exchange::Bithumb;
    
    // null 체크와 함께 값 가져오기
    if (!content["symbol"].is_null()) {
        ticker.symbol = content["symbol"].get<std::string>();
    } else {
        return;
    }
    
    // 가격 정보 파싱
    if (!content["closePrice"].is_null()) {
        ticker.price = std::stod(content["closePrice"].get<std::string>());
        // 빗썸 ticker는 호가 정보를 제공하지 않으므로 closePrice를 사용
        ticker.bid = ticker.price;
        ticker.ask = ticker.price;
    }
    
    // 거래량 정보
    if (content.contains("volume") && !content["volume"].is_null()) {
        ticker.volume_24h = std::stod(content["volume"].get<std::string>());
    }
    
    // 현재는 간단히 현재 시간 사용
    ticker.timestamp = std::chrono::system_clock::now();
    
    WebSocketEvent evt(WebSocketEvent::Type::Ticker, Exchange::Bithumb, ticker);
    
    logger_->info("[Bithumb] Ticker parsed - Symbol: {}, Price: {} KRW", 
                  ticker.symbol, ticker.price);
    
    emit_event(std::move(evt));
}

void BithumbWebSocket::parse_orderbook(const nlohmann::json& data) {
    if (!data.contains("content")) {
        logger_->warn("[Bithumb] No content field in orderbook data");
        return;
    }
    
    auto content = data["content"];
    
    // content가 배열인지 객체인지 확인
    if (content.is_array()) {
        // content가 배열인 경우 - 여러 심볼의 데이터
        for (const auto& item : content) {
            parse_single_orderbook(item);
        }
        return;
    }
    
    // content가 객체인 경우 - 단일 심볼의 데이터
    parse_single_orderbook(content);
}

void BithumbWebSocket::parse_single_orderbook(const nlohmann::json& content) {
    // 빗썸의 실제 orderbook 데이터 구조:
    // {"datetime":"...", "list":[{"orderType":"bid", "price":"...", "quantity":"...", "symbol":"..."}, ...]}
    
    if (!content.contains("list") || !content["list"].is_array()) {
        logger_->warn("[Bithumb] No list field in orderbook data: {}", content.dump());
        return;
    }
    
    // symbol별로 orderbook 데이터를 그룹화
    std::map<std::string, OrderBook> orderbooks;
    
    for (const auto& item : content["list"]) {
        if (!item.contains("symbol") || !item.contains("orderType") ||
            !item.contains("price") || !item.contains("quantity")) {
            continue;
        }
        
        std::string symbol = item["symbol"].get<std::string>();
        std::string orderType = item["orderType"].get<std::string>();
        
        // OrderBook 객체 생성 또는 가져오기
        if (orderbooks.find(symbol) == orderbooks.end()) {
            orderbooks[symbol] = OrderBook();
            orderbooks[symbol].exchange = Exchange::Bithumb;
            orderbooks[symbol].symbol = symbol;
            orderbooks[symbol].timestamp = std::chrono::system_clock::now();
        }
        
        PriceLevel level;
        level.price = std::stod(item["price"].get<std::string>());
        level.quantity = std::stod(item["quantity"].get<std::string>());
        
        if (orderType == "bid") {
            orderbooks[symbol].bids.push_back(level);
        } else if (orderType == "ask") {
            orderbooks[symbol].asks.push_back(level);
        }
    }
    
    // 각 심볼별로 이벤트 발생
    for (auto& [symbol, orderbook] : orderbooks) {
        // 가격순으로 정렬 (bid는 내림차순, ask는 오름차순)
        std::sort(orderbook.bids.begin(), orderbook.bids.end(),
                  [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
        std::sort(orderbook.asks.begin(), orderbook.asks.end(),
                  [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });
        
        // OrderBook 파싱 결과 출력
        if (!orderbook.bids.empty() && !orderbook.asks.empty()) {
            logger_->info("[Bithumb] OrderBook parsed - Symbol: {}, Best Bid: {} KRW, Best Ask: {} KRW",
                         orderbook.symbol, orderbook.bids[0].price, orderbook.asks[0].price);
        }
        
        WebSocketEvent evt(WebSocketEvent::Type::OrderBook, Exchange::Bithumb, orderbook);
        
        emit_event(std::move(evt));
    }
}

void BithumbWebSocket::parse_trade(const nlohmann::json& data) {
    if (!data.contains("content")) {
        return;
    }
    
    auto content = data["content"];
    
    // Bithumb는 transaction 데이터를 리스트로 제공
    if (content.contains("list") && content["list"].is_array()) {
        for (const auto& transaction : content["list"]) {
            Ticker trade;
            trade.exchange = Exchange::Bithumb;
            
            // 심볼
            if (transaction.contains("symbol")) {
                trade.symbol = transaction["symbol"];
            }
            
            // 체결가
            if (transaction.contains("contPrice")) {
                trade.price = std::stod(transaction["contPrice"].get<std::string>());
                // Bithumb trade에는 bid/ask 정보가 없으므로 체결가로 설정
                trade.bid = trade.price;
                trade.ask = trade.price;
            }
            
            // 체결량
            if (transaction.contains("contQty")) {
                trade.volume_24h = std::stod(transaction["contQty"].get<std::string>());
            }
            
            // 타임스탬프
            if (transaction.contains("contDtm")) {
                // Bithumb는 microsecond timestamp 사용
                int64_t timestamp_us = std::stoll(transaction["contDtm"].get<std::string>());
                trade.timestamp = std::chrono::system_clock::time_point(
                    std::chrono::microseconds(timestamp_us)
                );
            }
            
            logger_->info("[Bithumb] Trade parsed - Symbol: {}, Price: {} KRW, Volume: {}",
                         trade.symbol, trade.price, trade.volume_24h);
            
            WebSocketEvent evt(WebSocketEvent::Type::Trade, Exchange::Bithumb, trade);
            emit_event(std::move(evt));
        }
    }
}

void BithumbWebSocket::send_additional_subscriptions() {
    // orderbook 구독이 있고 아직 보내지 않았으면
    if (!orderbook_symbols_.empty() && !orderbook_subscribed_) {
        nlohmann::json msg = {
            {"type", "orderbookdepth"},
            {"symbols", orderbook_symbols_}
        };
        send_message(msg.dump());
        orderbook_subscribed_ = true;
    }
    
    // transaction 구독이 있고 아직 보내지 않았으면
    if (!trade_symbols_.empty() && !trade_subscribed_) {
        nlohmann::json msg = {
            {"type", "transaction"},
            {"symbols", trade_symbols_}
        };
        send_message(msg.dump());
        trade_subscribed_ = true;
    }
}

}  // namespace arbitrage