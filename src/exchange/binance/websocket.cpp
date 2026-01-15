#include "arbitrage/exchange/binance/websocket.hpp"
#include "arbitrage/common/json.hpp"
#include <chrono>
#include <algorithm>

namespace arbitrage {

BinanceWebSocket::BinanceWebSocket(net::io_context& ioc, ssl::context& ctx)
    : WebSocketClientBase(ioc, ctx, Exchange::Binance) {
}

void BinanceWebSocket::subscribe_ticker(const std::vector<std::string>& symbols) {
    for (const auto& symbol : symbols) {
        // 심볼을 소문자로 변환 (Binance는 소문자 사용)
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(), 
                      lower_symbol.begin(), ::tolower);
        streams_.push_back(lower_symbol + "@ticker");
    }
}

void BinanceWebSocket::subscribe_orderbook(const std::vector<std::string>& symbols, int depth) {
    orderbook_depth_ = depth;
    for (const auto& symbol : symbols) {
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(), 
                      lower_symbol.begin(), ::tolower);
        streams_.push_back(lower_symbol + "@depth" + std::to_string(depth));
    }
}

void BinanceWebSocket::subscribe_trade(const std::vector<std::string>& symbols) {
    for (const auto& symbol : symbols) {
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(), 
                      lower_symbol.begin(), ::tolower);
        streams_.push_back(lower_symbol + "@trade");
    }
}

void BinanceWebSocket::connect_with_streams() {
    // Combined Stream URL 구성
    std::string target = "/stream";
    if (!streams_.empty()) {
        target += "?streams=";
        for (size_t i = 0; i < streams_.size(); ++i) {
            if (i > 0) target += "/";
            target += streams_[i];
        }
    }
    
    connect("stream.binance.com", "9443", target);
}

std::string BinanceWebSocket::build_subscribe_message() {
    // Combined Stream 사용 시 별도 구독 메시지 불필요
    if (!streams_.empty()) {
        return "";
    }
    
    // 개별 구독 메시지 (대안)
    nlohmann::json subscribe_msg = {
        {"method", "SUBSCRIBE"},
        {"params", streams_},
        {"id", subscribe_id_++}
    };
    
    return subscribe_msg.dump();
}

void BinanceWebSocket::parse_message(const std::string& message) {
    try {
        auto json = nlohmann::json::parse(message);
        
        // Combined Stream 메시지 형식
        if (json.contains("stream") && json.contains("data")) {
            std::string stream = json["stream"];
            auto data = json["data"];
            
            // 스트림 이름에서 심볼 추출 (예: "btcusdt@depth" -> "BTCUSDT")
            std::string symbol;
            size_t at_pos = stream.find('@');
            if (at_pos != std::string::npos) {
                symbol = stream.substr(0, at_pos);
                std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
            }
            
            if (stream.find("@ticker") != std::string::npos) {
                parse_ticker(data);
            } else if (stream.find("@depth") != std::string::npos) {
                parse_orderbook(data, symbol);
            } else if (stream.find("@aggTrade") != std::string::npos ||
                       stream.find("@trade") != std::string::npos) {
                parse_trade(data);
            }
        }
        // 단일 스트림 메시지
        else if (json.contains("e")) {
            std::string event_type = json["e"];

            if (event_type == "24hrTicker") {
                parse_ticker(json);
            } else if (event_type == "depthUpdate") {
                parse_orderbook(json, "");
            } else if (event_type == "trade" || event_type == "aggTrade") {
                parse_trade(json);
            }
        }
        
    } catch (const std::exception& e) {
        logger_->error("[Binance] Parse error: {}", e.what());
    }
}

void BinanceWebSocket::parse_ticker(const nlohmann::json& data) {
    Ticker ticker;
    ticker.exchange = Exchange::Binance;
    ticker.symbol = data["s"];  // Symbol
    ticker.price = std::stod(data["c"].get<std::string>());  // Last price
    ticker.bid = std::stod(data["b"].get<std::string>());    // Best bid
    ticker.ask = std::stod(data["a"].get<std::string>());    // Best ask
    ticker.volume_24h = std::stod(data["v"].get<std::string>());  // Volume
    
    // Binance는 타임스탬프를 밀리초로 제공
    int64_t timestamp_ms = data["E"];  // Event time
    ticker.timestamp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(timestamp_ms)
    );
    
    logger_->info("[Binance] Ticker - Symbol: {}, Price: {} USDT",
                  ticker.symbol, ticker.price);

    WebSocketEvent evt(WebSocketEvent::Type::Ticker, Exchange::Binance, ticker);

    emit_event(std::move(evt));
}

void BinanceWebSocket::parse_orderbook(const nlohmann::json& data, const std::string& stream_symbol) {
    OrderBook orderbook;
    orderbook.exchange = Exchange::Binance;
    
    // 심볼 추출 (depthUpdate 이벤트에는 's' 필드가 없을 수 있음)
    if (data.contains("s")) {
        orderbook.symbol = data["s"];
    } else if (!stream_symbol.empty()) {
        orderbook.symbol = stream_symbol;
    }
    
    // Bids (매수 호가)
    if (data.contains("bids")) {
        for (const auto& bid : data["bids"]) {
            PriceLevel level;
            level.price = std::stod(bid[0].get<std::string>());
            level.quantity = std::stod(bid[1].get<std::string>());
            orderbook.bids.push_back(level);
        }
    }
    
    // Asks (매도 호가)
    if (data.contains("asks")) {
        for (const auto& ask : data["asks"]) {
            PriceLevel level;
            level.price = std::stod(ask[0].get<std::string>());
            level.quantity = std::stod(ask[1].get<std::string>());
            orderbook.asks.push_back(level);
        }
    }
    
    // 타임스탬프
    if (data.contains("E")) {
        int64_t timestamp_ms = data["E"];
        orderbook.timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(timestamp_ms)
        );
    } else {
        orderbook.timestamp = std::chrono::system_clock::now();
    }
    
    WebSocketEvent evt(WebSocketEvent::Type::OrderBook, Exchange::Binance, orderbook);
    
    emit_event(std::move(evt));
}

void BinanceWebSocket::parse_trade(const nlohmann::json& data) {
    Ticker trade;
    trade.exchange = Exchange::Binance;
    
    // 심볼 (대문자 변환)
    std::string symbol = data["s"];
    trade.symbol = symbol;
    
    // 체결가
    trade.price = std::stod(data["p"].get<std::string>());
    
    // Binance trade에는 bid/ask 정보가 없으므로 체결가로 설정
    trade.bid = trade.price;
    trade.ask = trade.price;
    
    // 체결량
    trade.volume_24h = std::stod(data["q"].get<std::string>());
    
    // 타임스탬프
    int64_t timestamp_ms = data["T"];
    trade.timestamp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(timestamp_ms)
    );
    
    logger_->info("[Binance] Trade - Symbol: {}, Price: {} USDT",
                  trade.symbol, trade.price);

    WebSocketEvent evt(WebSocketEvent::Type::Trade, Exchange::Binance, trade);

    emit_event(std::move(evt));
}

}  // namespace arbitrage