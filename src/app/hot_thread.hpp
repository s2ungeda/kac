#pragma once

/**
 * Hot Thread Classes (Core-Pinned, Busy-Poll)
 *
 * - HotThreadWS: WebSocket mode (Standalone) - consumes from SPSC queue
 * - HotThreadShm: SHM mode (Engine) - consumes from 4 SHM ring buffers
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/lockfree_queue.hpp"
#include "arbitrage/executor/types.hpp"
#include "arbitrage/ipc/shm_manager.hpp"
#include "arbitrage/ipc/shm_ring_buffer.hpp"
#include "arbitrage/ipc/shm_latest.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <thread>

namespace arbitrage {

// Forward declarations
class SimpleLogger;
class EventBus;
class PremiumCalculator;
class DecisionEngine;
class OrderBookAnalyzer;
struct WebSocketEvent;

// =============================================================================
// ShmFeedQueue — holds one SHM consumer for a single exchange
// =============================================================================
struct ShmFeedQueue {
    Exchange exchange{};
    std::unique_ptr<ShmSegment> segment;
    ShmRingBuffer<Ticker> queue;
    std::unique_ptr<ShmSegment> ob_segment;
    ShmLatestValue<OrderBook> ob_slot;
};

// =============================================================================
// HotThreadWS — Standalone WebSocket mode
// =============================================================================
class HotThreadWS {
public:
    struct Deps {
        SPSCQueue<WebSocketEvent>* ws_queue;
        PremiumCalculator* calculator;
        DecisionEngine* engine;
        SPSCQueue<DualOrderRequest>* order_queue;
        std::shared_ptr<EventBus> event_bus;
        std::atomic<double>* price_upbit;
        std::atomic<double>* price_bithumb;
        std::atomic<double>* price_binance;
        std::atomic<double>* price_mexc;
        std::shared_ptr<SimpleLogger> logger;
        bool has_executor;
        std::atomic<bool>* running;
    };

    explicit HotThreadWS(Deps deps);

    void start();
    void stop();
    void join();

    /// Run loop (public so Application can call from its own thread lambda)
    void run();

private:
    Deps deps_;
    std::thread thread_;
};

// =============================================================================
// HotThreadShm — Engine SHM mode
// =============================================================================
class HotThreadShm {
public:
    struct Deps {
        std::array<ShmFeedQueue, 4>* shm_feeds;
        PremiumCalculator* calculator;
        DecisionEngine* engine;
        SPSCQueue<DualOrderRequest>* order_queue;
        std::shared_ptr<EventBus> event_bus;
        OrderBookAnalyzer* ob_analyzer;
        std::atomic<double>* price_upbit;
        std::atomic<double>* price_bithumb;
        std::atomic<double>* price_binance;
        std::atomic<double>* price_mexc;
        std::shared_ptr<SimpleLogger> logger;
        bool has_executor;
        std::atomic<bool>* running;
    };

    explicit HotThreadShm(Deps deps);

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
