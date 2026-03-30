#pragma once

/**
 * Order Thread (Cold Path - blocking REST API calls)
 */

#include "arbitrage/common/lockfree_queue.hpp"
#include "arbitrage/executor/types.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace arbitrage {

// Forward declarations
class SimpleLogger;
class EventBus;
class DualOrderExecutor;
class DecisionEngine;
class DailyLossLimiter;
class TradingStatsTracker;

// =============================================================================
// OrderThread
// =============================================================================
class OrderThread {
public:
    struct Deps {
        SPSCQueue<DualOrderRequest>* order_queue;
        DualOrderExecutor* executor;
        DecisionEngine* engine;
        DailyLossLimiter* daily_limit;
        TradingStatsTracker* stats_tracker;
        std::shared_ptr<EventBus> event_bus;
        std::atomic<double>* current_fx_rate;
        std::shared_ptr<SimpleLogger> logger;
        std::atomic<bool>* running;
    };

    explicit OrderThread(Deps deps);

    void start();
    void stop();
    void join();

    /// Run loop (public so Application can call from its own thread lambda)
    void run();

private:
    Deps deps_;
    std::thread thread_;
};

}  // namespace arbitrage
