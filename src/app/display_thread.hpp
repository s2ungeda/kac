#pragma once

/**
 * Display Thread & FX Rate Thread (Cold Path)
 *
 * DisplayThread: periodic price summary and premium matrix output
 * FxRateThread: periodic FX rate updates
 *
 * Utility functions: get_timestamp(), print_matrix()
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/spin_wait.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace arbitrage {

// Forward declarations
class SimpleLogger;
class PremiumCalculator;
class FXRateService;

// =============================================================================
// Utility functions
// =============================================================================
std::string get_timestamp();
void print_matrix(const PremiumMatrix& matrix);

// =============================================================================
// DisplayThread
// =============================================================================
class DisplayThread {
public:
    struct Deps {
        PremiumCalculator* calculator;
        std::atomic<double>* price_upbit;
        std::atomic<double>* price_bithumb;
        std::atomic<double>* price_binance;
        std::atomic<double>* price_mexc;
        std::atomic<double>* current_fx_rate;
        std::atomic<bool>* wakeup;
        std::atomic<bool>* running_for_wakeup;
        std::atomic<bool>* running;
    };

    explicit DisplayThread(Deps deps);

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
// FxRateThread
// =============================================================================
class FxRateThread {
public:
    struct Deps {
        FXRateService* fx_service;
        PremiumCalculator* calculator;
        std::atomic<double>* current_fx_rate;
        std::shared_ptr<SimpleLogger> logger;
        std::atomic<bool>* wakeup;
        std::atomic<bool>* running_for_wakeup;
        std::atomic<bool>* running;
    };

    explicit FxRateThread(Deps deps);

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
