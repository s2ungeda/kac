#pragma once

/**
 * Application Class
 *
 * Owns all infrastructure, trading components, and thread lifecycle.
 * Refactored from the monolithic main() function.
 */

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/lockfree_queue.hpp"
#include "arbitrage/executor/types.hpp"
#include "arbitrage/ipc/order_channel.hpp"

namespace arbitrage {

// Forward declarations
class SimpleLogger;
class EventBus;
class PremiumCalculator;
class DecisionEngine;
class RiskModel;
class FXRateService;
class OrderBookAnalyzer;
class FeeCalculator;
class DualOrderExecutor;
class RecoveryManager;
class OrderClientBase;
class ConfigWatcher;
class TcpServer;
class UnixSocketClient;
class UpbitWebSocket;
class BinanceWebSocket;
class BithumbWebSocket;
class MEXCWebSocket;

struct WebSocketEvent;
struct ShmFeedQueue;

// =============================================================================
// Run Mode & Options (declared in main.cpp, passed to Application)
// =============================================================================
enum class RunMode {
    Standalone,  // Phase 1: WebSocket direct connection
    Engine       // Phase 2: SHM mode (external Feeder processes)
};

struct AppOptions {
    std::string config_path = "config/config.yaml";
    RunMode mode = RunMode::Standalone;
    bool dry_run = false;
    bool verbose = false;
    bool config_from_stdin = false;
};

// =============================================================================
// Application Class
// =============================================================================
class Application {
public:
    explicit Application(const AppOptions& opts);
    ~Application();

    // Non-copyable, non-movable
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Main entry point. Returns exit code.
    int run();

private:
    // --- Initialization phases (correspond to main() sections 1-10) ---
    int  init_logger();
    void init_infra();
    int  init_trading();
    int  init_network();
    void init_order_execution();
    void init_cold_services();
    void init_queues_and_bridge();
    void register_shutdown_handlers();
    int  subscribe_and_connect();

    // --- Runtime ---
    void start_threads();
    void wait_for_shutdown();
    void cleanup();
    void print_final_stats();

    // --- Options ---
    AppOptions opts_;
    bool engine_mode_;

    // --- Logger ---
    std::shared_ptr<SimpleLogger> logger_;

    // --- Thread control ---
    std::atomic<bool> running_{true};
    std::mutex cv_mutex_;
    std::condition_variable cv_shutdown_;

    // --- Infrastructure ---
    std::shared_ptr<EventBus> event_bus_;

    // --- Trading ---
    std::unique_ptr<OrderBookAnalyzer> ob_analyzer_;
    std::unique_ptr<RiskModel> risk_model_;
    std::unique_ptr<DecisionEngine> decision_engine_;
    std::unique_ptr<FXRateService> fx_service_;
    std::unique_ptr<PremiumCalculator> calculator_;

    // --- Network (Standalone mode) ---
    boost::asio::io_context ioc_;
    boost::asio::ssl::context ssl_ctx_;
    std::shared_ptr<UpbitWebSocket>   upbit_ws_;
    std::shared_ptr<BinanceWebSocket> binance_ws_;
    std::shared_ptr<BithumbWebSocket> bithumb_ws_;
    std::shared_ptr<MEXCWebSocket>    mexc_ws_;

    // --- SHM (Engine mode) ---
    std::unique_ptr<std::array<ShmFeedQueue, 4>> shm_feeds_;

    // --- Price atomics (for display thread) ---
    std::atomic<double> price_upbit_{0};
    std::atomic<double> price_bithumb_{0};
    std::atomic<double> price_binance_{0};
    std::atomic<double> price_mexc_{0};
    std::atomic<double> current_fx_rate_{0};

    // --- Queues ---
    std::unique_ptr<SPSCQueue<WebSocketEvent>> ws_queue_;
    std::unique_ptr<SPSCQueue<DualOrderRequest>> order_queue_;

    // --- Order Execution ---
    std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients_;
    std::shared_ptr<RecoveryManager> recovery_mgr_;
    std::unique_ptr<DualOrderExecutor> dual_executor_;
    OrderChannel order_channel_;
    std::unique_ptr<UnixSocketClient> monitor_client_;

    // --- Cold Services ---
    std::unique_ptr<ConfigWatcher> config_watcher_;
    std::unique_ptr<TcpServer> tcp_server_;

    // --- Threads ---
    std::thread io_thread_;
    std::thread hot_thread_;
    std::thread order_thread_;
    std::thread display_thread_;
    std::thread fx_thread_;

    // --- Shutdown result ---
    struct ShutdownResult {
        std::chrono::milliseconds total_elapsed{0};
        std::vector<std::string> completed_components;
        std::vector<std::string> timeout_components;
    };
    ShutdownResult shutdown_result_;
};

}  // namespace arbitrage
