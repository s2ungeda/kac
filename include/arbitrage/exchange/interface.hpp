#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/error.hpp"
#include <functional>
#include <memory>
#include <future>

namespace arbitrage {

// 콜백 타입
using TickerCallback = std::function<void(const Ticker&)>;
using OrderBookCallback = std::function<void(const OrderBook&)>;
using OrderCallback = std::function<void(const OrderResult&)>;

// 거래소 인터페이스
class IExchange {
public:
    virtual ~IExchange() = default;
    
    // 거래소 식별
    virtual Exchange name() const = 0;
    
    // 연결 관리
    virtual Result<void> connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    
    // 구독
    virtual void subscribe_ticker(const std::string& symbol, TickerCallback cb) = 0;
    virtual void subscribe_orderbook(const std::string& symbol, OrderBookCallback cb) = 0;
    virtual void unsubscribe(const std::string& symbol) = 0;
    
    // 주문
    virtual std::future<Result<OrderResult>> place_order(const OrderRequest& req) = 0;
    virtual std::future<Result<OrderResult>> cancel_order(const std::string& order_id) = 0;
    virtual std::future<Result<OrderResult>> get_order(const std::string& order_id) = 0;
    
    // 잔고
    virtual std::future<Result<std::map<std::string, Balance>>> get_balances() = 0;
    
    // RTT 측정
    virtual std::future<Result<Duration>> ping() = 0;
    
    // 이벤트 루프 (blocking)
    virtual void run() = 0;
    
    // 이벤트 루프 (non-blocking, 한 번 실행)
    virtual void poll() = 0;
    
    // 이벤트 루프 중지
    virtual void stop() = 0;
};

// 거래소 팩토리
std::unique_ptr<IExchange> create_exchange(Exchange ex);

}  // namespace arbitrage