#include "arbitrage/executor/recovery.hpp"
#include <thread>
#include <iostream>

namespace arbitrage {

// =============================================================================
// RecoveryManager
// =============================================================================

RecoveryManager::RecoveryManager(
    std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients)
    : order_clients_(std::move(order_clients))
{
}

RecoveryManager::~RecoveryManager() {
}

// =============================================================================
// 복구 계획 생성
// =============================================================================

RecoveryPlan RecoveryManager::create_plan(
    const DualOrderRequest& request,
    const DualOrderResult& result)
{
    ++stats_.total_plans;

    bool buy_ok = result.buy_result.is_success();
    bool sell_ok = result.sell_result.is_success();

    // 둘 다 성공 → 복구 불필요
    if (buy_ok && sell_ok) {
        return RecoveryPlan{RecoveryAction::None, {}, "Both orders succeeded"};
    }

    // 둘 다 실패 → 복구 불필요
    if (!buy_ok && !sell_ok) {
        return RecoveryPlan{RecoveryAction::None, {},
                           "Both orders failed, no recovery needed"};
    }

    // 매수 성공, 매도 실패 → 손절 매도
    if (buy_ok && !sell_ok) {
        ++stats_.sell_bought_plans;
        return create_sell_bought_plan(request, result);
    }

    // 매수 실패, 매도 성공 → 매수 복구
    if (!buy_ok && sell_ok) {
        ++stats_.buy_sold_plans;
        return create_buy_sold_plan(request, result);
    }

    // 그 외 (이론적으로 도달 불가)
    ++stats_.manual_plans;
    return RecoveryPlan{RecoveryAction::ManualIntervention, {},
                       "Unexpected state, manual intervention required"};
}

RecoveryPlan RecoveryManager::create_sell_bought_plan(
    const DualOrderRequest& request,
    const DualOrderResult& result)
{
    RecoveryPlan plan;
    plan.action = RecoveryAction::SellBought;
    plan.reason = "Buy succeeded but Sell failed, liquidating bought position";
    plan.max_retries = max_retries_;
    plan.retry_delay = retry_delay_;

    // 매수한 거래소에서 손절 매도
    // 시장가 주문으로 즉시 청산
    plan.recovery_order.exchange = request.buy_order.exchange;
    plan.recovery_order.set_symbol(request.buy_order.symbol);
    plan.recovery_order.side = OrderSide::Sell;
    plan.recovery_order.type = OrderType::Market;
    plan.recovery_order.quantity = result.buy_result.filled_qty();
    plan.recovery_order.price = 0.0;  // Market order

    return plan;
}

RecoveryPlan RecoveryManager::create_buy_sold_plan(
    const DualOrderRequest& request,
    const DualOrderResult& result)
{
    RecoveryPlan plan;
    plan.action = RecoveryAction::BuySold;
    plan.reason = "Sell succeeded but Buy failed, covering sold position";
    plan.max_retries = max_retries_;
    plan.retry_delay = retry_delay_;

    // 매도한 거래소에서 매수 복구
    // 시장가 주문으로 즉시 포지션 커버
    plan.recovery_order.exchange = request.sell_order.exchange;
    plan.recovery_order.set_symbol(request.sell_order.symbol);
    plan.recovery_order.side = OrderSide::Buy;
    plan.recovery_order.type = OrderType::Market;
    plan.recovery_order.quantity = result.sell_result.filled_qty();
    plan.recovery_order.price = 0.0;  // Market order

    return plan;
}

// =============================================================================
// 복구 실행
// =============================================================================

RecoveryResult RecoveryManager::execute_recovery(const RecoveryPlan& plan) {
    RecoveryPlan mutable_plan = plan;
    return execute_with_retry(mutable_plan);
}

std::future<RecoveryResult> RecoveryManager::execute_recovery_async(
    const RecoveryPlan& plan)
{
    return std::async(std::launch::async, [this, plan]() {
        return execute_recovery(plan);
    });
}

void RecoveryManager::execute_recovery_with_callback(
    const RecoveryPlan& plan,
    RecoveryCompleteCallback callback)
{
    std::thread([this, plan, callback = std::move(callback)]() {
        auto result = execute_recovery(plan);
        if (callback) {
            callback(result);
        }
    }).detach();
}

RecoveryResult RecoveryManager::execute_with_retry(RecoveryPlan& plan) {
    RecoveryResult result;
    result.success = false;

    if (!plan.needs_execution()) {
        result.plan = plan;
        result.message = "No execution needed";
        return result;
    }

    ++stats_.executions;

    while (plan.can_retry()) {
        if (dry_run_) {
            // Dry run 모드 - 실제 주문 없이 성공 반환
            result.plan = plan;
            result.success = true;
            result.message = "Dry run - no actual execution";
            ++stats_.successes;
            return result;
        }

        result.result = execute_order(plan.recovery_order);

        if (result.result.is_success()) {
            result.plan = plan;
            result.success = true;
            result.message = "Recovery order executed successfully";
            ++stats_.successes;
            return result;
        }

        // 실패 - 재시도
        plan.increment_retry();
        ++stats_.retries;

        if (plan.can_retry()) {
            std::this_thread::sleep_for(plan.retry_delay);
        }
    }

    // 최대 재시도 초과
    ++stats_.failures;
    result.plan = plan;
    result.success = false;
    result.message = "Recovery failed after " +
                    std::to_string(plan.max_retries) + " attempts";

    return result;
}

SingleOrderResult RecoveryManager::execute_order(const OrderRequest& order) {
    SingleOrderResult result;
    result.exchange = order.exchange;
    result.start_time = std::chrono::steady_clock::now();

    // 주문 클라이언트 획득
    auto it = order_clients_.find(order.exchange);
    if (it == order_clients_.end()) {
        result.error = Error{
            ErrorCode::InvalidRequest,
            "Exchange not configured: " + std::string(exchange_name(order.exchange))
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
// RecoveryQueue
// =============================================================================

RecoveryQueue::RecoveryQueue(std::shared_ptr<RecoveryManager> manager)
    : manager_(std::move(manager))
{
}

RecoveryQueue::~RecoveryQueue() {
    stop();
}

void RecoveryQueue::enqueue(const RecoveryPlan& plan) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(plan);
    }
    cv_.notify_one();
}

void RecoveryQueue::start() {
    if (running_.exchange(true)) {
        return;  // Already running
    }

    worker_ = std::thread([this]() {
        worker_loop();
    });
}

void RecoveryQueue::stop() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }
}

size_t RecoveryQueue::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void RecoveryQueue::worker_loop() {
    while (running_) {
        RecoveryPlan plan;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return !running_ || !queue_.empty();
            });

            if (!running_ && queue_.empty()) {
                break;
            }

            if (queue_.empty()) {
                continue;
            }

            plan = std::move(queue_.front());
            queue_.pop();
        }

        // 복구 실행
        auto result = manager_->execute_recovery(plan);

        // 콜백 호출
        if (callback_) {
            callback_(result);
        }
    }
}

}  // namespace arbitrage
