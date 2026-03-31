/**
 * Display Thread & FX Rate Thread Implementation (Cold Path)
 *
 * DisplayThread: periodic price summary and premium matrix display
 * FxRateThread: periodic FX rate updates from external service
 */

#include "display_thread.hpp"

#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/common/spin_wait.hpp"
#include "arbitrage/strategy/premium_calc.hpp"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <cmath>

namespace arbitrage {

// Display interval constants (match defaults:: in main.cpp)
static constexpr int DISPLAY_INTERVAL_SEC  = 10;
static constexpr int MATRIX_INTERVAL_SEC   = 30;
static constexpr int FX_UPDATE_INTERVAL_SEC = 30;

// =============================================================================
// Utility Functions
// =============================================================================

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void print_matrix(const PremiumMatrix& matrix) {
    const char* exchanges[] = {"Upbit", "Bithumb", "Binance", "MEXC"};
    std::cout << "\n=== Premium Matrix (%) ===\n\n";
    std::cout << "       Buy ->\n";
    std::cout << "Sell v ";
    for (int i = 0; i < 4; ++i) std::cout << std::setw(9) << exchanges[i];
    std::cout << "\n";
    for (int sell = 0; sell < 4; ++sell) {
        std::cout << std::setw(7) << exchanges[sell];
        for (int buy = 0; buy < 4; ++buy) {
            double premium = matrix[buy][sell];
            std::cout << std::setw(9) << std::fixed << std::setprecision(2);
            if (std::isnan(premium)) std::cout << "N/A";
            else if (premium > 3.0) std::cout << "*" << premium << "*";
            else std::cout << premium;
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

// =============================================================================
// DisplayThread
// =============================================================================

DisplayThread::DisplayThread(Deps deps)
    : deps_(std::move(deps))
{}

void DisplayThread::start() {
    thread_ = std::thread(&DisplayThread::run, this);
}

void DisplayThread::stop() {
    // running_ flag is controlled by Application
}

void DisplayThread::join() {
    if (thread_.joinable()) thread_.join();
}

void DisplayThread::run() {
    int cycle = 0;
    while (deps_.running->load(std::memory_order_relaxed)) {
        SpinWait::until_for([this] {
            return !deps_.running_for_wakeup->load(std::memory_order_acquire) ||
                   deps_.wakeup->load(std::memory_order_acquire);
        }, std::chrono::seconds(1));
        deps_.wakeup->store(false, std::memory_order_release);
        if (!deps_.running->load(std::memory_order_relaxed)) break;

        cycle++;

        // Every 10 seconds: price summary
        if (cycle % DISPLAY_INTERVAL_SEC == 0) {
            std::cout << "[" << get_timestamp() << "] "
                      << "Upbit: " << std::fixed << std::setprecision(0)
                      << deps_.price_upbit->load()
                      << " | Bithumb: " << deps_.price_bithumb->load()
                      << " | Binance: " << std::setprecision(4)
                      << deps_.price_binance->load()
                      << " | MEXC: " << deps_.price_mexc->load()
                      << " | FX: " << std::setprecision(2)
                      << deps_.current_fx_rate->load()
                      << "\n";
        }

        // Every 30 seconds: matrix
        if (cycle % MATRIX_INTERVAL_SEC == 0) {
            print_matrix(deps_.calculator->get_matrix());
        }
    }
}

// =============================================================================
// FxRateThread
// =============================================================================

FxRateThread::FxRateThread(Deps deps)
    : deps_(std::move(deps))
{}

void FxRateThread::start() {
    thread_ = std::thread(&FxRateThread::run, this);
}

void FxRateThread::stop() {
    // running_ flag is controlled by Application
}

void FxRateThread::join() {
    if (thread_.joinable()) thread_.join();
}

void FxRateThread::run() {
    while (deps_.running->load(std::memory_order_relaxed)) {
        auto fx = deps_.fx_service->fetch();
        if (fx) {
            double rate = fx.value().rate;
            deps_.current_fx_rate->store(rate, std::memory_order_relaxed);
            deps_.calculator->update_fx_rate(rate);
            deps_.logger->debug("FX rate updated: {:.2f} ({})", rate, fx.value().source);
        }

        SpinWait::until_for([this] {
            return !deps_.running_for_wakeup->load(std::memory_order_acquire) ||
                   deps_.wakeup->load(std::memory_order_acquire);
        }, std::chrono::seconds(FX_UPDATE_INTERVAL_SEC));
        deps_.wakeup->store(false, std::memory_order_release);
    }
}

}  // namespace arbitrage
