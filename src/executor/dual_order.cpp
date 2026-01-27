#include "arbitrage/executor/dual_order.hpp"
#include "arbitrage/executor/recovery.hpp"
#include <thread>
#include <iostream>

namespace arbitrage {

// 간단한 조건부 로깅 (릴리즈 빌드에서 비활성화 가능)
#ifndef NDEBUG
#define EXEC_LOG(msg) std::cerr << "[Executor] " << msg << std::endl
#else
#define EXEC_LOG(msg) ((void)0)
#endif

// =============================================================================
// 생성자/소멸자
// =============================================================================

DualOrderExecutor::DualOrderExecutor(
    std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients,
    std::shared_ptr<RecoveryManager> recovery)
    : order_clients_(std::move(order_clients))
    , recovery_(std::move(recovery))
{
}

DualOrderExecutor::~DualOrderExecutor() {
}

// =============================================================================
// 주문 실행
// =============================================================================

DualOrderResult DualOrderExecutor::execute_sync(const DualOrderRequest& request) {
    DualOrderResult result;
    result.request_id = request.request_id;
    result.start_time = std::chrono::steady_clock::now();

    // 요청 유효성 검사
    if (!request.is_valid()) {
        result.buy_result.error = Error{ErrorCode::InvalidRequest, "Invalid request"};
        result.sell_result.error = Error{ErrorCode::InvalidRequest, "Invalid request"};
        result.end_time = std::chrono::steady_clock::now();
        return result;
    }

    // 거래소 클라이언트 확인
    if (!has_exchange(request.buy_order.exchange)) {
        result.buy_result.error = Error{
            ErrorCode::InvalidRequest,
            "Buy exchange not configured"
        };
    }
    if (!has_exchange(request.sell_order.exchange)) {
        result.sell_result.error = Error{
            ErrorCode::InvalidRequest,
            "Sell exchange not configured"
        };
    }

    if (result.buy_result.error || result.sell_result.error) {
        result.end_time = std::chrono::steady_clock::now();
        return result;
    }

    // =========================================================================
    // 핵심: std::async 병렬 실행
    // =========================================================================
    auto buy_future = std::async(std::launch::async, [this, &request]() {
        return execute_single(
            request.buy_order.exchange,
            request.buy_order,
            request.buy_delay
        );
    });

    auto sell_future = std::async(std::launch::async, [this, &request]() {
        return execute_single(
            request.sell_order.exchange,
            request.sell_order,
            request.sell_delay
        );
    });

    // 결과 대기
    result.buy_result = buy_future.get();
    result.sell_result = sell_future.get();
    result.end_time = std::chrono::steady_clock::now();

    // 통계 기록
    stats_.record_result(result);

    // 부분 체결 시 자동 복구
    if (result.partial_fill() && auto_recovery_ && recovery_) {
        handle_recovery(request, result);
    }

    return result;
}

std::future<DualOrderResult> DualOrderExecutor::execute_async(
    const DualOrderRequest& request)
{
    return std::async(std::launch::async, [this, request]() {
        return execute_sync(request);
    });
}

void DualOrderExecutor::execute_with_callback(
    const DualOrderRequest& request,
    DualOrderCallback callback)
{
    std::thread([this, request, callback = std::move(callback)]() {
        auto result = execute_sync(request);
        if (callback) {
            callback(result);
        }
    }).detach();
}

// =============================================================================
// 개별 주문 실행
// =============================================================================

SingleOrderResult DualOrderExecutor::execute_single(
    Exchange exchange,
    const OrderRequest& order,
    Duration delay)
{
    SingleOrderResult result;
    result.exchange = exchange;
    result.start_time = std::chrono::steady_clock::now();

    // RTT 보정 지연
    if (delay.count() > 0) {
        std::this_thread::sleep_for(delay);
    }

    // 주문 클라이언트 획득
    auto it = order_clients_.find(exchange);
    if (it == order_clients_.end()) {
        result.error = Error{
            ErrorCode::InvalidRequest,
            "Exchange not configured: " + std::string(exchange_name(exchange))
        };
        result.end_time = std::chrono::steady_clock::now();
        result.latency = std::chrono::duration_cast<Duration>(
            result.end_time - result.start_time);
        return result;
    }

    // 주문 실행
    auto order_result = it->second->place_order(order);

    result.end_time = std::chrono::steady_clock::now();
    result.latency = std::chrono::duration_cast<Duration>(
        result.end_time - result.start_time);

    if (order_result.has_value()) {
        result.result = std::move(order_result.value());
    } else {
        result.error = order_result.error();
    }

    return result;
}

// =============================================================================
// 복구 처리
// =============================================================================

void DualOrderExecutor::handle_recovery(
    const DualOrderRequest& request,
    DualOrderResult& result)
{
    if (!recovery_) {
        return;
    }

    // 복구 계획 생성
    auto plan = recovery_->create_plan(request, result);

    if (plan.action == RecoveryAction::None) {
        return;
    }

    if (plan.action == RecoveryAction::ManualIntervention) {
        if (recovery_callback_) {
            RecoveryResult recovery_result;
            recovery_result.plan = plan;
            recovery_result.success = false;
            recovery_result.message = "Manual intervention required";
            recovery_callback_(recovery_result);
        }
        return;
    }

    // 복구 실행
    auto recovery_result = recovery_->execute_recovery(plan);

    // 통계 기록
    stats_.record_recovery(recovery_result.success);

    // 콜백 호출
    if (recovery_callback_) {
        recovery_callback_(recovery_result);
    }
}

// =============================================================================
// 설정
// =============================================================================

void DualOrderExecutor::set_recovery_manager(
    std::shared_ptr<RecoveryManager> recovery)
{
    recovery_ = std::move(recovery);
}

std::vector<Exchange> DualOrderExecutor::supported_exchanges() const {
    std::vector<Exchange> exchanges;
    exchanges.reserve(order_clients_.size());
    for (const auto& [ex, client] : order_clients_) {
        exchanges.push_back(ex);
    }
    return exchanges;
}

}  // namespace arbitrage
