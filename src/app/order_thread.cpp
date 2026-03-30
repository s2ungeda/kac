/**
 * Order Thread Implementation (Cold Path)
 *
 * Consumes DualOrderRequest from SPSC queue, executes blocking REST API calls.
 */

#include "order_thread.hpp"

#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/spin_wait.hpp"
#include "arbitrage/common/thread_config.hpp"
#include "arbitrage/common/thread_manager.hpp"
#include "arbitrage/executor/dual_order.hpp"
#include "arbitrage/strategy/decision_engine.hpp"
#include "arbitrage/ops/daily_limit.hpp"
#include "arbitrage/ops/trading_stats.hpp"
#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"

namespace arbitrage {

OrderThread::OrderThread(Deps deps)
    : deps_(std::move(deps))
{}

void OrderThread::start() {
    thread_ = std::thread(&OrderThread::run, this);
}

void OrderThread::stop() {
    // running_ flag is controlled by Application
}

void OrderThread::join() {
    if (thread_.joinable()) thread_.join();
}

void OrderThread::run() {
    // Pin to core 2
    ThreadConfig order_cfg;
    order_cfg.name = "order_thread";
    order_cfg.core_id = 2;
    order_cfg.priority = ThreadPriority::High;
    ThreadManager::apply_to_current(order_cfg);

    AdaptiveSpinWait waiter;
    DualOrderRequest request;
    uint64_t order_count = 0;

    while (deps_.running->load(std::memory_order_relaxed)) {
        if (deps_.order_queue->pop(request)) {
            waiter.reset();
            order_count++;

            // DualOrderStarted event
            events::DualOrderStarted start_evt;
            start_evt.buy_exchange = request.buy_order.exchange;
            start_evt.sell_exchange = request.sell_order.exchange;
            start_evt.quantity = request.buy_order.quantity;
            deps_.event_bus->publish(start_evt);

            deps_.logger->info("Executing dual order #{}: buy {} sell {}",
                order_count,
                exchange_name(request.buy_order.exchange),
                exchange_name(request.sell_order.exchange));

            // Execute order (blocking - REST API call, 100ms+)
            auto result = deps_.executor->execute_sync(request);

            // P&L calculation
            double fx_rate = deps_.current_fx_rate->load(std::memory_order_relaxed);
            double profit = result.gross_profit(fx_rate);

            // Record results
            deps_.engine->record_trade_result(profit);
            deps_.daily_limit->record_trade(profit);
            deps_.stats_tracker->record_trade(profit);

            // DualOrderCompleted event
            events::DualOrderCompleted complete_evt;
            complete_evt.success = result.both_filled();
            complete_evt.actual_profit = profit;
            deps_.event_bus->publish(complete_evt);

            if (result.both_filled()) {
                deps_.logger->info("Dual order #{} filled: profit={:.0f} KRW",
                    order_count, profit);
                deps_.engine->start_cooldown(deps_.engine->config().cooldown_after_trade);
            } else if (result.partial_fill()) {
                deps_.logger->warn("Dual order #{} partial fill — recovery needed", order_count);
                deps_.engine->start_cooldown(deps_.engine->config().cooldown_after_loss);
            } else {
                deps_.logger->error("Dual order #{} failed", order_count);
                deps_.engine->start_cooldown(deps_.engine->config().cooldown_after_loss);
            }
        } else {
            waiter.wait();
        }
    }

    deps_.logger->info("Order thread stopped: {} orders executed", order_count);
}

}  // namespace arbitrage
