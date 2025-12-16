#include "arbitrage/exchange/mexc/websocket.hpp"
#include "arbitrage/exchange/mexc/protobuf_parser.hpp"
#include "arbitrage/common/json.hpp"
#include <chrono>
#include <algorithm>
#include <cctype>
#include <thread>
#include <fstream>

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
    logger_->debug("[MEXC] Connected, starting subscriptions...");
    logger_->debug("[MEXC] Ticker symbols: {}, Trade symbols: {}", 
                   ticker_symbols_.size(), trade_symbols_.size());
    
    // 구독 상태 초기화 (재연결 시 필요)
    ticker_idx_ = 0;
    orderbook_idx_ = 0;
    trade_idx_ = 0;
    ticker_done_ = false;
    orderbook_done_ = false;
    trade_done_ = false;
    
    // 첫 구독 시작
    send_delayed_subscriptions();
}

std::string MEXCWebSocket::build_subscribe_message() {
    // MEXC는 on_connected에서 처리하므로 여기서는 빈 문자열 반환
    return "";
}

void MEXCWebSocket::send_delayed_subscriptions() {
    // 하나의 구독만 전송
    nlohmann::json sub_msg;
    
    if (!orderbook_done_ && orderbook_idx_ < orderbook_symbols_.size()) {
        // MEXC는 대문자 심볼을 요구함
        std::string symbol = orderbook_symbols_[orderbook_idx_];
        std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
        
        // 오더북 채널
        sub_msg = {
            {"method", "SUBSCRIPTION"},
            {"params", nlohmann::json::array({
                "spot@public.aggre.depth.v3.api.pb@100ms@" + symbol
            })},
            {"id", subscribe_id_++}
        };
        logger_->debug("[MEXC] Subscribing to orderbook: {}", sub_msg.dump());
        orderbook_idx_++;
        if (orderbook_idx_ >= orderbook_symbols_.size()) orderbook_done_ = true;
    } else if (!ticker_done_ && ticker_idx_ < ticker_symbols_.size()) {
        // MEXC는 대문자 심볼을 요구함
        std::string symbol = ticker_symbols_[ticker_idx_];
        std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
        
        // miniTicker 채널 (timezone 포함)
        sub_msg = {
            {"method", "SUBSCRIPTION"},
            {"params", nlohmann::json::array({
                "spot@public.miniTicker.v3.api.pb@" + symbol + "@UTC+8"
            })},
            {"id", subscribe_id_++}
        };
        logger_->debug("[MEXC] Subscribing to ticker: {}", sub_msg.dump());
        ticker_idx_++;
        if (ticker_idx_ >= ticker_symbols_.size()) ticker_done_ = true;
    } else if (!trade_done_ && trade_idx_ < trade_symbols_.size()) {
        // MEXC는 대문자 심볼을 요구함
        std::string symbol = trade_symbols_[trade_idx_];
        std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
        
        // 실제 동작하는 MEXC 형식 (델파이 테스트 기반)
        sub_msg = {
            {"method", "SUBSCRIPTION"},
            {"params", nlohmann::json::array({
                "spot@public.aggre.deals.v3.api.pb@100ms@" + symbol
            })},
            {"id", subscribe_id_++}
        };
        logger_->debug("[MEXC] Subscribing to trades: {}", sub_msg.dump());
        trade_idx_++;
        if (trade_idx_ >= trade_symbols_.size()) trade_done_ = true;
    }
    
    if (!sub_msg.empty()) {
        logger_->debug("[MEXC] Sending subscription for channel");
        send_message(sub_msg.dump());
    } else {
        logger_->debug("[MEXC] All subscriptions sent");
    }
}

void MEXCWebSocket::parse_message(const std::string& message) {
    try {
        // 첫 메시지 디버깅 - 바이너리 데이터일 수 있음
        if (message.empty()) {
            logger_->debug("[MEXC] Received empty message");
            return;
        }
        
        // 바이너리 데이터인지 확인
        bool is_binary = false;
        for (unsigned char c : message) {
            if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
                is_binary = true;
                break;
            }
        }
        
        if (is_binary) {
            // protobuf 디코딩 시도
            try {
                auto fields = mexc::ProtobufParser::parse(message);
                std::string channel;
                std::string symbol;
                
                // Parse wrapper fields
                for (const auto& field : fields) {
                    if (field.tag == 1 && field.wire_type == mexc::ProtobufParser::LENGTH_DELIMITED) {
                        channel = field.data;
                    } else if (field.tag == 3 && field.wire_type == mexc::ProtobufParser::LENGTH_DELIMITED) {
                        symbol = field.data;
                    } else if (field.tag == 313 && field.wire_type == mexc::ProtobufParser::LENGTH_DELIMITED) {
                        // Field 313 contains the actual orderbook data
                        if (channel.find("aggre.depth") != std::string::npos) {
                            parse_orderbook_protobuf(field.data, symbol);
                        }
                    } else if (field.tag == 314 && field.wire_type == mexc::ProtobufParser::LENGTH_DELIMITED) {
                        // Field 314 contains the actual trade data
                        if (channel.find("aggre.deals") != std::string::npos) {
                            parse_trade_protobuf(field.data, symbol);
                        }
                    }
                }
            } catch (const std::exception& e) {
                logger_->error("[MEXC] Failed to parse protobuf: {}", e.what());
            }
            return;
        }
        
        auto json = nlohmann::json::parse(message);
        logger_->debug("[MEXC] Received JSON: {}", message.substr(0, 200));
        
        // 구독 응답 처리
        if (json.contains("code")) {
            if (json.contains("msg")) {
                std::string msg = json["msg"];
                if (msg.find("Not Subscribed successfully") != std::string::npos) {
                    logger_->warn("[MEXC] Subscription failed: {}", msg);
                    // 실패해도 다음 구독 시도
                    send_delayed_subscriptions();
                } else {
                    logger_->debug("[MEXC] Subscription response: {}", msg);
                    // 성공 시 다음 구독 전송
                    send_delayed_subscriptions();
                }
            }
            return;
        }
        
        // Ping 응답 처리
        if (json.contains("ping")) {
            int64_t ping_value = json["ping"].get<int64_t>();
            std::string pong_msg = "{\"pong\":" + std::to_string(ping_value) + "}";
            logger_->debug("[MEXC] Received ping: {}, sending pong", ping_value);
            send_message(pong_msg);
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

void MEXCWebSocket::parse_ticker_protobuf(const std::string& data) {
    auto ticker_opt = mexc::parseMiniTicker(data);
    if (!ticker_opt) {
        logger_->error("[MEXC] Failed to parse ticker protobuf");
        return;
    }
    
    const auto& mexc_ticker = ticker_opt.value();
    
    Ticker ticker;
    ticker.exchange = Exchange::MEXC;
    ticker.symbol = mexc_ticker.symbol;
    ticker.price = mexc_ticker.price;
    ticker.bid = mexc_ticker.price; // miniTicker doesn't have bid/ask
    ticker.ask = mexc_ticker.price;
    ticker.volume_24h = mexc_ticker.volume;
    ticker.timestamp = std::chrono::system_clock::now();
    
    logger_->debug("[MEXC] Ticker parsed - Symbol: {}, Price: {}, Volume: {}",
                  ticker.symbol, ticker.price, ticker.volume_24h);
    
    WebSocketEvent evt(WebSocketEvent::Type::Ticker, Exchange::MEXC, ticker);
    emit_event(std::move(evt));
}

void MEXCWebSocket::parse_orderbook_protobuf(const std::string& data, const std::string& symbol) {
    auto orderbook_opt = mexc::parseAggreDepth(data);
    if (!orderbook_opt) {
        logger_->error("[MEXC] Failed to parse orderbook protobuf");
        return;
    }
    
    const auto& mexc_ob = orderbook_opt.value();
    
    OrderBook orderbook;
    orderbook.exchange = Exchange::MEXC;
    orderbook.symbol = symbol;
    
    // Convert asks
    for (const auto& ask : mexc_ob.asks) {
        PriceLevel level;
        level.price = ask.price;
        level.quantity = ask.quantity;
        orderbook.asks.push_back(level);
    }
    
    // Convert bids
    for (const auto& bid : mexc_ob.bids) {
        PriceLevel level;
        level.price = bid.price;
        level.quantity = bid.quantity;
        orderbook.bids.push_back(level);
    }
    
    orderbook.timestamp = std::chrono::system_clock::now();
    
    if (!orderbook.bids.empty() && !orderbook.asks.empty()) {
        logger_->debug("[MEXC] OrderBook parsed - Best Bid: {}, Best Ask: {}",
                      orderbook.bids[0].price, orderbook.asks[0].price);
    }
    
    WebSocketEvent evt(WebSocketEvent::Type::OrderBook, Exchange::MEXC, orderbook);
    emit_event(std::move(evt));
}

void MEXCWebSocket::parse_trade_protobuf(const std::string& data, const std::string& symbol) {
    auto trades_opt = mexc::parseAggreDeals(data);
    if (!trades_opt) {
        logger_->error("[MEXC] Failed to parse trades protobuf");
        return;
    }
    
    const auto& trades = trades_opt.value();
    
    for (const auto& mexc_trade : trades) {
        Ticker trade;
        trade.exchange = Exchange::MEXC;
        trade.symbol = symbol;
        trade.price = mexc_trade.price;
        trade.bid = mexc_trade.price;
        trade.ask = mexc_trade.price;
        trade.volume_24h = mexc_trade.quantity;
        trade.timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(mexc_trade.timestamp)
        );
        
        logger_->debug("[MEXC] Trade parsed - Price: {}, Volume: {}, Type: {}",
                      trade.price, trade.volume_24h, 
                      mexc_trade.trade_type == 0 ? "Buy" : "Sell");
        
        WebSocketEvent evt(WebSocketEvent::Type::Trade, Exchange::MEXC, trade);
        emit_event(std::move(evt));
    }
}

}  // namespace arbitrage