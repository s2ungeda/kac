#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/error.hpp"
#include <string>
#include <memory>

namespace arbitrage {

// 공통 주문 인터페이스
class OrderClientBase {
public:
    virtual ~OrderClientBase() = default;
    
    // 주문
    virtual Result<OrderResult> place_order(const OrderRequest& req) = 0;
    virtual Result<OrderResult> cancel_order(const std::string& order_id) = 0;
    virtual Result<OrderResult> get_order(const std::string& order_id) = 0;
    
    // 잔고
    virtual Result<Balance> get_balance(const std::string& currency) = 0;
    
    // 거래소 정보
    virtual Exchange exchange() const = 0;
    virtual std::string name() const = 0;
    
protected:
    // Rate Limiter (공통) - TASK_08에서 구현 예정
    void acquire_rate_limit() {
        // TODO: rate_limits().acquire(exchange(), ApiType::Order);
    }
    
    bool try_acquire_rate_limit() {
        // TODO: return rate_limits().try_acquire(exchange(), ApiType::Order);
        return true;
    }
};

// 팩토리 함수
std::unique_ptr<OrderClientBase> create_order_client(
    Exchange exchange,
    const std::string& api_key,
    const std::string& secret_key);

}  // namespace arbitrage