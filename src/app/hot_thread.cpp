/**
 * Hot Thread Implementation
 *
 * Core-pinned busy-poll threads for low-latency market data processing.
 */

#include "hot_thread.hpp"

#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/spin_wait.hpp"
#include "arbitrage/common/thread_config.hpp"
#include "arbitrage/common/thread_manager.hpp"
#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/strategy/decision_engine.hpp"
#include "arbitrage/strategy/orderbook_analyzer.hpp"
#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"
#include "arbitrage/exchange/websocket_base.hpp"

namespace arbitrage {

// Default hot thread core (matches defaults::HOT_THREAD_CORE in main.cpp)
static constexpr int HOT_THREAD_CORE = 1;
static constexpr double MIN_EVALUATE_PREMIUM = 0.1;

// =============================================================================
// HotThreadWS
// =============================================================================

HotThreadWS::HotThreadWS(Deps deps)
    : deps_(std::move(deps))
{}

void HotThreadWS::start() {
    thread_ = std::thread(&HotThreadWS::run, this);
}

void HotThreadWS::stop() {
    // running_ flag is controlled by Application
}

void HotThreadWS::join() {
    if (thread_.joinable()) thread_.join();
}

void HotThreadWS::run() {
    // Core pinning + high priority
    ThreadConfig hot_cfg;
    hot_cfg.name = "hot_thread";
    hot_cfg.core_id = HOT_THREAD_CORE;
    hot_cfg.priority = ThreadPriority::High;
    auto apply_result = ThreadManager::apply_to_current(hot_cfg);
    if (apply_result) {
        deps_.logger->info("Hot thread: core {} pinned, priority High", HOT_THREAD_CORE);
    } else {
        deps_.logger->warn("Hot thread: failed to apply config ({})", apply_result.error().message);
    }

    AdaptiveSpinWait waiter;
    WebSocketEvent event;
    uint64_t tick_count = 0;
    uint64_t eval_count = 0;

    while (deps_.running->load(std::memory_order_relaxed)) {
        bool had_work = false;

        // 1. SPSC Queue drain
        while (deps_.ws_queue->pop(event)) {
            had_work = true;

            if (event.is_ticker() || event.is_trade()) {
                double price = event.ticker().price;
                Exchange ex = event.exchange;

                // 2. Premium calculator update
                deps_.calculator->update_price(ex, price);

                // Display thread atomic update
                switch (ex) {
                    case Exchange::Upbit:   deps_.price_upbit->store(price, std::memory_order_relaxed); break;
                    case Exchange::Bithumb: deps_.price_bithumb->store(price, std::memory_order_relaxed); break;
                    case Exchange::Binance: deps_.price_binance->store(price, std::memory_order_relaxed); break;
                    case Exchange::MEXC:    deps_.price_mexc->store(price, std::memory_order_relaxed); break;
                    default: break;
                }

                tick_count++;
            }
        }

        // 3. Opportunity evaluation
        if (had_work) {
            auto best = deps_.calculator->get_best_opportunity();
            if (best && best->premium_pct > MIN_EVALUATE_PREMIUM) {
                auto decision = deps_.engine->evaluate(*best);
                eval_count++;

                if (decision.should_execute() && deps_.has_executor) {
                    // 4. Push to order queue
                    deps_.order_queue->push(decision.order_request);

                    // Cold path event
                    events::OpportunityDetected opp_evt;
                    opp_evt.premium_pct = best->premium_pct;
                    opp_evt.buy_exchange = best->buy_exchange;
                    opp_evt.sell_exchange = best->sell_exchange;
                    opp_evt.recommended_qty = decision.optimal_qty;
                    opp_evt.expected_profit = decision.expected_profit_krw;
                    opp_evt.confidence = decision.confidence;
                    deps_.event_bus->publish(opp_evt);

                    deps_.logger->info("ORDER QUEUED: {:.2f}% {} -> {} qty={:.1f}",
                        best->premium_pct,
                        exchange_name(best->buy_exchange),
                        exchange_name(best->sell_exchange),
                        decision.optimal_qty);
                }
            }
        }

        // 5. Adaptive spin
        if (!had_work) {
            waiter.wait();
        } else {
            waiter.reset();
        }
    }

    deps_.logger->info("Hot thread stopped: {} ticks, {} evaluations", tick_count, eval_count);
}

// =============================================================================
// HotThreadShm
// =============================================================================

HotThreadShm::HotThreadShm(Deps deps)
    : deps_(std::move(deps))
{}

void HotThreadShm::start() {
    thread_ = std::thread(&HotThreadShm::run, this);
}

void HotThreadShm::stop() {
    // running_ flag is controlled by Application
}

void HotThreadShm::join() {
    if (thread_.joinable()) thread_.join();
}

void HotThreadShm::run() {
    // Core pinning + high priority
    ThreadConfig hot_cfg;
    hot_cfg.name = "hot_thread_shm";
    hot_cfg.core_id = HOT_THREAD_CORE;
    hot_cfg.priority = ThreadPriority::High;
    auto apply_result = ThreadManager::apply_to_current(hot_cfg);
    if (apply_result) {
        deps_.logger->info("Hot thread (SHM): core {} pinned, priority High", HOT_THREAD_CORE);
    } else {
        deps_.logger->warn("Hot thread (SHM): failed to apply config ({})", apply_result.error().message);
    }

    AdaptiveSpinWait waiter;
    Ticker ticker;
    OrderBook orderbook;
    uint64_t tick_count = 0;
    uint64_t ob_count = 0;
    uint64_t eval_count = 0;

    while (deps_.running->load(std::memory_order_relaxed)) {
        bool had_work = false;

        // Round-robin poll 4 SHM queues
        for (auto& feed : *deps_.shm_feeds) {
            // Ticker queue
            while (feed.queue.pop(ticker)) {
                had_work = true;
                tick_count++;

                double price = ticker.price;
                Exchange ex = ticker.exchange;

                deps_.calculator->update_price(ex, price);

                switch (ex) {
                    case Exchange::Upbit:   deps_.price_upbit->store(price, std::memory_order_relaxed); break;
                    case Exchange::Bithumb: deps_.price_bithumb->store(price, std::memory_order_relaxed); break;
                    case Exchange::Binance: deps_.price_binance->store(price, std::memory_order_relaxed); break;
                    case Exchange::MEXC:    deps_.price_mexc->store(price, std::memory_order_relaxed); break;
                    default: break;
                }
            }

            // OrderBook — latest value (seqlock)
            if (feed.ob_slot.load(orderbook)) {
                had_work = true;
                ob_count++;
                deps_.ob_analyzer->update(feed.exchange, orderbook);
            }

            // Feeder process disconnect detection
            if (feed.queue.valid() && feed.queue.is_closed() && !feed.queue.is_producer_alive()) {
                deps_.logger->warn("Feeder {} disconnected (SHM closed)",
                    exchange_name(feed.exchange));
            }
        }

        // Opportunity evaluation
        if (had_work) {
            auto best = deps_.calculator->get_best_opportunity();
            if (best && best->premium_pct > MIN_EVALUATE_PREMIUM) {
                auto decision = deps_.engine->evaluate(*best);
                eval_count++;

                if (decision.should_execute() && deps_.has_executor) {
                    deps_.order_queue->push(decision.order_request);

                    events::OpportunityDetected opp_evt;
                    opp_evt.premium_pct = best->premium_pct;
                    opp_evt.buy_exchange = best->buy_exchange;
                    opp_evt.sell_exchange = best->sell_exchange;
                    opp_evt.recommended_qty = decision.optimal_qty;
                    opp_evt.expected_profit = decision.expected_profit_krw;
                    opp_evt.confidence = decision.confidence;
                    deps_.event_bus->publish(opp_evt);

                    deps_.logger->info("ORDER QUEUED: {:.2f}% {} -> {} qty={:.1f}",
                        best->premium_pct,
                        exchange_name(best->buy_exchange),
                        exchange_name(best->sell_exchange),
                        decision.optimal_qty);
                }
            }
        }

        // Adaptive spin
        if (!had_work) {
            waiter.wait();
        } else {
            waiter.reset();
        }
    }

    deps_.logger->info("Hot thread (SHM) stopped: {} ticks, {} orderbooks, {} evaluations",
                 tick_count, ob_count, eval_count);
}

}  // namespace arbitrage
