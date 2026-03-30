/**
 * Application Class Implementation
 *
 * Refactored from main() — owns all infrastructure and thread lifecycle.
 * Sections 1-14 from the original main() are grouped into init/run/shutdown methods.
 */

// === Core ===
#include "arbitrage/app/application.hpp"
#include "arbitrage/common/runtime_keystore.hpp"
#include "arbitrage/common/config.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/common/fee_calculator.hpp"
#include "arbitrage/common/symbol_master.hpp"
#include "arbitrage/common/config_watcher.hpp"
#include "arbitrage/common/lockfree_queue.hpp"

// === Strategy ===
#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/strategy/decision_engine.hpp"
#include "arbitrage/strategy/risk_model.hpp"
#include "arbitrage/strategy/orderbook_analyzer.hpp"

// === Executor ===
#include "arbitrage/executor/dual_order.hpp"
#include "arbitrage/executor/recovery.hpp"

// === Infra ===
#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"
#include "arbitrage/infra/shutdown.hpp"
#include "arbitrage/infra/health_check.hpp"
#include "arbitrage/infra/tcp_server.hpp"
#include "arbitrage/infra/watchdog_client.hpp"

// === IPC ===
#include "arbitrage/ipc/ipc_types.hpp"
#include "arbitrage/ipc/ipc_protocol.hpp"
#include "arbitrage/ipc/shm_manager.hpp"
#include "arbitrage/ipc/shm_queue.hpp"
#include "arbitrage/ipc/shm_ring_buffer.hpp"
#include "arbitrage/ipc/shm_latest.hpp"
#include "arbitrage/ipc/order_channel.hpp"
#include "arbitrage/ipc/unix_socket.hpp"

// === Ops ===
#include "arbitrage/ops/alert.hpp"
#include "arbitrage/ops/daily_limit.hpp"
#include "arbitrage/ops/trading_stats.hpp"

// === Exchange ===
#include "arbitrage/exchange/websocket_base.hpp"
#include "arbitrage/exchange/upbit/websocket.hpp"
#include "arbitrage/exchange/binance/websocket.hpp"
#include "arbitrage/exchange/bithumb/websocket.hpp"
#include "arbitrage/exchange/mexc/websocket.hpp"
#include "arbitrage/exchange/order_base.hpp"

// === App threads ===
#include "hot_thread.hpp"
#include "order_thread.hpp"
#include "display_thread.hpp"

// === System ===
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cstdlib>

namespace arbitrage {

// =============================================================================
// Constants (from namespace defaults in original main.cpp)
// =============================================================================
namespace defaults {
    constexpr double FX_RATE_FALLBACK      = 1450.0;
    constexpr double PREMIUM_ALERT_PCT     = 2.0;
    constexpr int    EVENTBUS_WORKERS      = 2;
    constexpr size_t WS_QUEUE_CAPACITY     = 4096;
    constexpr size_t ORDER_QUEUE_CAPACITY  = 64;
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

Application::Application(const AppOptions& opts)
    : opts_(opts)
    , engine_mode_(opts.mode == RunMode::Engine)
    , ssl_ctx_(boost::asio::ssl::context::tlsv12_client)
{
    ssl_ctx_.set_default_verify_paths();
}

Application::~Application() = default;

// =============================================================================
// run() — main entry point
// =============================================================================
int Application::run() {
    // Section 1: Logger & Config
    int rc = init_logger();
    if (rc != 0) return rc;

    // Section 2: System Infrastructure
    init_infra();

    // Section 3: Trading Infrastructure
    rc = init_trading();
    if (rc != 0) return rc;

    // Section 4a/4b: Network / SHM
    rc = init_network();
    if (rc != 0) return rc;

    // Section 5: Market data services (FX, Premium)
    // Section 6: Order execution
    init_order_execution();

    // Section 7: Cold services
    init_cold_services();

    // Section 8: SPSC Queues + Data Source Bridge
    init_queues_and_bridge();

    // Section 9: Shutdown handlers
    register_shutdown_handlers();

    // Section 10: Symbol subscription & connect
    rc = subscribe_and_connect();
    if (rc != 0) return rc;

    // Section 11: Start threads
    start_threads();

    // Section 12: Wait for shutdown
    wait_for_shutdown();

    // Section 13: Cleanup threads
    cleanup();

    // Section 14: Final stats
    print_final_stats();

    Logger::shutdown();
    std::cout << "\nGoodbye!\n";

    // Detached service threads may still be shutting down
    _exit(0);
}

// =============================================================================
// Section 1: Logger & Config
// =============================================================================
int Application::init_logger() {
    Logger::init("logs");
    logger_ = Logger::create("main");

    const char* mode_str = engine_mode_ ? "ENGINE (SHM)" : "STANDALONE";

    std::cout << "==============================================\n";
    std::cout << "   Kimchi Arbitrage System (C++)\n";
    std::cout << "   Mode: " << mode_str << "\n";
    if (opts_.dry_run) std::cout << "   *** DRY RUN MODE ***\n";
    std::cout << "==============================================\n\n";

    logger_->info("Starting Kimchi Arbitrage System (mode={})", mode_str);
    if (opts_.dry_run) logger_->warn("DRY RUN MODE - No real orders will be placed");

    if (opts_.config_from_stdin) {
        logger_->info("Config: stdin (SOPS pipeline)");
        if (!Config::instance().load_from_stream(std::cin)) {
            logger_->error("Failed to load config from stdin");
            return 1;
        }
    } else {
        logger_->info("Config: {}", opts_.config_path);
        Config::instance().load(opts_.config_path);
    }

    // Process security hardening (disable core dumps)
    RuntimeKeyStore::harden_process();

    return 0;
}

// =============================================================================
// Section 2: System Infrastructure
// =============================================================================
void Application::init_infra() {
    // Shutdown Manager (signal handlers)
    auto& shutdown_mgr = ShutdownManager::instance();
    shutdown_mgr.install_signal_handlers();

    // EventBus (async event processing)
    event_bus_ = EventBus::instance();
    event_bus_->start_async(defaults::EVENTBUS_WORKERS);
    shutdown_mgr.set_event_bus(event_bus_);
}

// =============================================================================
// Section 3: Trading Infrastructure
// =============================================================================
int Application::init_trading() {
    // Symbol Master
    auto& sym_master = symbol_master();
    sym_master.init_xrp_defaults();

    // Fee Calculator
    auto& fee_calc = fee_calculator();

    // OrderBook Analyzer
    ob_analyzer_ = std::make_unique<OrderBookAnalyzer>(&fee_calc);

    // Risk Model
    risk_model_ = std::make_unique<RiskModel>();
    risk_model_->set_fee_calculator(&fee_calc);

    // Decision Engine
    StrategyConfig strategy_config;
    decision_engine_ = std::make_unique<DecisionEngine>(strategy_config);
    decision_engine_->set_risk_model(risk_model_.get());

    return 0;
}

// =============================================================================
// Section 4a/4b: Network (Standalone: WebSocket) / SHM (Engine)
// =============================================================================
int Application::init_network() {
    // --- 4a. Standalone: WebSocket direct connection ---
    if (!engine_mode_) {
        upbit_ws_   = std::make_shared<UpbitWebSocket>(ioc_, ssl_ctx_);
        binance_ws_ = std::make_shared<BinanceWebSocket>(ioc_, ssl_ctx_);
        bithumb_ws_ = std::make_shared<BithumbWebSocket>(ioc_, ssl_ctx_);
        mexc_ws_    = std::make_shared<MEXCWebSocket>(ioc_, ssl_ctx_);
    }

    // --- 4b. Engine: SHM queue attach ---
    constexpr size_t SHM_CAPACITY = 4096;
    constexpr size_t SHM_SIZE = shm_queue_size(SHM_CAPACITY, sizeof(Ticker));

    shm_feeds_ = std::make_unique<std::array<ShmFeedQueue, 4>>();
    const size_t OB_SHM_SIZE = shm_latest_size<OrderBook>();

    if (engine_mode_) {
        const Exchange exchanges[] = {
            Exchange::Upbit, Exchange::Bithumb, Exchange::Binance, Exchange::MEXC
        };
        for (size_t i = 0; i < 4; ++i) {
            Exchange ex = exchanges[i];
            const char* name = shm_names::feed_name(ex);
            const char* ob_name = shm_names::ob_name(ex);
            (*shm_feeds_)[i].exchange = ex;

            // Ticker SHM (RingBuffer)
            try {
                (*shm_feeds_)[i].segment = std::make_unique<ShmSegment>(name, SHM_SIZE, false);
            } catch (const std::runtime_error& e) {
                logger_->error("Failed to attach SHM: {} — is {}-feeder running? ({})",
                    name, exchange_name(ex), e.what());
                std::cerr << "ERROR: Cannot attach to " << name
                          << ". Start " << exchange_name(ex) << "-feeder first.\n";
                return 1;
            }
            (*shm_feeds_)[i].queue = ShmRingBuffer<Ticker>::attach_consumer(
                (*shm_feeds_)[i].segment->data());
            if (!(*shm_feeds_)[i].queue.valid()) {
                logger_->error("Invalid SHM RingBuffer: {} — check version/type mismatch", name);
                std::cerr << "ERROR: SHM " << name << " is invalid.\n";
                return 1;
            }
            logger_->info("Ticker RingBuffer attached: {} (capacity={}, producer={})",
                name, (*shm_feeds_)[i].queue.capacity(), (*shm_feeds_)[i].queue.producer_pid());

            // OrderBook SHM (LatestValue)
            try {
                (*shm_feeds_)[i].ob_segment = std::make_unique<ShmSegment>(ob_name, OB_SHM_SIZE, false);
            } catch (const std::runtime_error& e) {
                logger_->error("Failed to attach OrderBook SHM: {} — is {}-feeder running? ({})",
                    ob_name, exchange_name(ex), e.what());
                std::cerr << "ERROR: Cannot attach to " << ob_name
                          << ". Start " << exchange_name(ex) << "-feeder first.\n";
                return 1;
            }
            (*shm_feeds_)[i].ob_slot = ShmLatestValue<OrderBook>::attach_consumer(
                (*shm_feeds_)[i].ob_segment->data());
            if (!(*shm_feeds_)[i].ob_slot.valid()) {
                logger_->error("Invalid OrderBook SHM: {} — check version/type mismatch", ob_name);
                std::cerr << "ERROR: OrderBook SHM " << ob_name << " is invalid.\n";
                return 1;
            }
            logger_->info("OrderBook SHM slot attached: {} (producer={})",
                ob_name, (*shm_feeds_)[i].ob_slot.producer_pid());
        }
        logger_->info("SHM mode: 4 feed queues + orderbook queues attached");
    }

    return 0;
}

// =============================================================================
// Section 5 + 6: Market Data Services + Order Execution
// =============================================================================
void Application::init_order_execution() {
    // --- Section 5: Market data services ---
    fx_service_ = std::make_unique<FXRateService>();
    calculator_ = std::make_unique<PremiumCalculator>();

    // Initial FX rate load
    {
        auto fx_result = fx_service_->fetch();
        double rate = fx_result ? fx_result.value().rate : defaults::FX_RATE_FALLBACK;
        current_fx_rate_.store(rate);
        calculator_->update_fx_rate(rate);
        std::cout << "FX Rate: " << std::fixed << std::setprecision(2) << rate
                  << " KRW/USD"
                  << (fx_result ? " (" + fx_result.value().source + ")" : " (default)")
                  << "\n";
    }

    // Premium callback
    calculator_->set_threshold(defaults::PREMIUM_ALERT_PCT);
    calculator_->on_premium_changed([this](const PremiumInfo& info) {
        logger_->warn("PREMIUM ALERT: {:.2f}% ({} -> {})",
                     info.premium_pct,
                     exchange_name(info.buy_exchange),
                     exchange_name(info.sell_exchange));
    });

    // --- Section 6: Order execution ---
    // Engine mode: orders go through OrderChannel(SHM) -> order-manager process
    // Standalone mode: in-process DualOrderExecutor (needs API keys)

    if (engine_mode_) {
        // SHM OrderChannel — Engine is request producer
        try {
            order_channel_ = OrderChannel::create_engine_side(defaults::ORDER_QUEUE_CAPACITY);
            if (order_channel_.request_queue_valid()) {
                logger_->info("OrderChannel (SHM) initialized");
            }
        } catch (const std::exception& e) {
            logger_->warn("OrderChannel init failed (non-fatal): {}", e.what());
        }

        // Monitor UDS client
        monitor_client_ = std::make_unique<UnixSocketClient>();
        if (monitor_client_->connect(ipc_paths::MONITOR_SOCKET)) {
            monitor_client_->start_recv();
            logger_->info("Connected to monitor: {}", ipc_paths::MONITOR_SOCKET);
        } else {
            logger_->warn("Monitor not available (non-fatal)");
        }
    } else {
        // Standalone: in-process executor
        if (!order_clients_.empty()) {
            recovery_mgr_ = std::make_shared<RecoveryManager>(order_clients_);
            dual_executor_ = std::make_unique<DualOrderExecutor>(order_clients_, recovery_mgr_);
            if (opts_.dry_run) {
                recovery_mgr_->set_dry_run(true);
            }
            logger_->info("Order execution enabled ({} exchanges)", order_clients_.size());
        } else {
            logger_->info("Monitor-only mode (no API keys configured)");
        }
    }
}

// =============================================================================
// Section 7: Cold Services
// =============================================================================
void Application::init_cold_services() {
    // Health Checker
    auto& health = health_checker();
    if (engine_mode_) {
        // Engine mode: only register Feeder health checks
        for (size_t i = 0; i < 4; ++i) {
            auto& feed = (*shm_feeds_)[i];
            std::string name = std::string(exchange_name(feed.exchange)) + "_feeder";
            health.register_check(name, [&feed, name]() -> ComponentHealth {
                ComponentHealth h;
                h.name = name;
                if (!feed.queue.valid()) {
                    h.status = HealthStatus::Unhealthy;
                } else if (feed.queue.is_closed() || !feed.queue.is_producer_alive()) {
                    h.status = HealthStatus::Unhealthy;
                } else {
                    h.status = HealthStatus::Healthy;
                }
                return h;
            });
        }
    } else {
        // Standalone mode: WebSocket connection health
        health.register_check("upbit_ws", [this]() -> ComponentHealth {
            ComponentHealth h;
            h.name = "upbit_ws";
            h.status = upbit_ws_->is_connected() ? HealthStatus::Healthy : HealthStatus::Unhealthy;
            return h;
        });
        health.register_check("binance_ws", [this]() -> ComponentHealth {
            ComponentHealth h;
            h.name = "binance_ws";
            h.status = binance_ws_->is_connected() ? HealthStatus::Healthy : HealthStatus::Unhealthy;
            return h;
        });
        health.register_check("bithumb_ws", [this]() -> ComponentHealth {
            ComponentHealth h;
            h.name = "bithumb_ws";
            h.status = bithumb_ws_->is_connected() ? HealthStatus::Healthy : HealthStatus::Unhealthy;
            return h;
        });
        health.register_check("mexc_ws", [this]() -> ComponentHealth {
            ComponentHealth h;
            h.name = "mexc_ws";
            h.status = mexc_ws_->is_connected() ? HealthStatus::Healthy : HealthStatus::Unhealthy;
            return h;
        });
    }
    health.register_check("decision_engine", [this]() -> ComponentHealth {
        ComponentHealth h;
        h.name = "decision_engine";
        h.status = decision_engine_->is_kill_switch_active()
            ? HealthStatus::Degraded : HealthStatus::Healthy;
        return h;
    });
    health.set_event_bus(event_bus_);

    // --- Cold services (Standalone only) ---
    auto& daily_limit = daily_limiter();
    auto& stats = trading_stats();
    if (!engine_mode_) {
        // Daily Loss Limiter
        daily_limit.set_kill_switch([this, &daily_limit]() {
            decision_engine_->set_kill_switch(true);
            auto st = daily_limit.get_stats();
            decision_engine_->set_kill_switch_reason("Daily loss limit reached");
            logger_->error("KILL SWITCH: Daily loss limit reached ({:.0f} KRW)", st.realized_pnl);
        });
        daily_limit.set_event_bus(event_bus_);
        daily_limit.start();

        // Alert Service
        auto& alerts = alert_service();
        alerts.set_event_bus(event_bus_);
        alerts.start();

        // EventBus subscriptions
        event_bus_->subscribe<events::KillSwitchActivated>(
            [&alerts](const events::KillSwitchActivated& e) {
                alerts.critical("Kill Switch", e.reason);
            });

        event_bus_->subscribe<events::DailyLossLimitReached>(
            [&alerts](const events::DailyLossLimitReached& e) {
                alerts.warning("Daily Loss Limit",
                    "Loss: " + std::to_string(static_cast<int>(e.loss_amount)) +
                    " / Limit: " + std::to_string(static_cast<int>(e.limit)));
            });

        health.on_unhealthy([this, &alerts](const ComponentHealth& ch) {
            alerts.error("Health Check", ch.name + " is unhealthy");
            logger_->error("UNHEALTHY: {}", ch.name);
        });
        health.start_periodic_check(std::chrono::seconds(30));

        event_bus_->subscribe<events::ExchangeDisconnected>(
            [&alerts](const events::ExchangeDisconnected& e) {
                alerts.error("Exchange Disconnected",
                    std::string(exchange_name(e.exchange)) + ": " + e.reason);
            });

        // Trading Stats
        event_bus_->subscribe<events::DualOrderCompleted>(
            [&stats](const events::DualOrderCompleted& e) {
                stats.record_trade(e.actual_profit);
            });
        stats.start();
    } else {
        logger_->info("Engine mode: Cold services delegated to monitor/risk-manager");
    }

    // Config Watcher (both modes)
    config_watcher_ = std::make_unique<ConfigWatcher>(
        opts_.config_path, std::chrono::milliseconds(5000));
    config_watcher_->on_reload([this]() {
        logger_->info("Config reloaded: {}", opts_.config_path);
        Config::instance().load(opts_.config_path);
        events::ConfigReloaded evt;
        evt.config_path = opts_.config_path;
        event_bus_->publish(evt);
    });
    config_watcher_->start();

    // TCP Server (Standalone only)
    if (!engine_mode_) {
        TcpServerConfig tcp_config;
        tcp_config.port = 9090;
        tcp_server_ = std::make_unique<TcpServer>(tcp_config);
        tcp_server_->set_event_bus(event_bus_);
        auto tcp_result = tcp_server_->start();
        if (tcp_result) {
            logger_->info("TCP server started on port {}", tcp_config.port);
        } else {
            logger_->warn("TCP server failed to start (non-fatal)");
        }
    }

    // Watchdog Client
    auto& watchdog = watchdog_client();
    auto& shutdown_mgr = ShutdownManager::instance();
    watchdog.on_command([this, &shutdown_mgr](WatchdogCommand cmd, const std::string& /*payload*/) {
        switch (cmd) {
            case WatchdogCommand::Shutdown:
                shutdown_mgr.initiate_shutdown("Watchdog requested shutdown");
                break;
            case WatchdogCommand::KillSwitch:
                decision_engine_->set_kill_switch(true);
                decision_engine_->set_kill_switch_reason("Watchdog kill switch");
                break;
            case WatchdogCommand::ReloadConfig:
                config_watcher_->reload();
                break;
            default:
                break;
        }
    });
    if (watchdog.connect()) {
        watchdog.start_heartbeat();
        logger_->info("Watchdog connected");
    } else {
        logger_->info("Watchdog not available (standalone mode)");
    }
}

// =============================================================================
// Section 8: SPSC Queues + Data Source Bridge
// =============================================================================
void Application::init_queues_and_bridge() {
    ws_queue_ = std::make_unique<SPSCQueue<WebSocketEvent>>(defaults::WS_QUEUE_CAPACITY);
    order_queue_ = std::make_unique<SPSCQueue<DualOrderRequest>>(defaults::ORDER_QUEUE_CAPACITY);

    if (!engine_mode_) {
        // Standalone: WebSocket callbacks -> in-process SPSC Queue
        upbit_ws_->on_event([this](const WebSocketEvent& evt) {
            if (evt.is_ticker() || evt.is_trade()) {
                ws_queue_->push(evt);
            }
        });

        binance_ws_->on_event([this](const WebSocketEvent& evt) {
            if (evt.is_ticker() || evt.is_trade()) {
                ws_queue_->push(evt);
            }
        });

        bithumb_ws_->on_event([this](const WebSocketEvent& evt) {
            if (evt.is_ticker() || evt.is_trade()) {
                ws_queue_->push(evt);
            }
        });

        mexc_ws_->on_event([this](const WebSocketEvent& evt) {
            if (evt.is_ticker() || evt.is_trade()) {
                ws_queue_->push(evt);
            }
        });
    }
    // Engine mode: SHM queues are already attached in init_network()
}

// =============================================================================
// Section 9: Shutdown Handlers
// =============================================================================
void Application::register_shutdown_handlers() {
    auto& shutdown_mgr = ShutdownManager::instance();

    if (engine_mode_) {
        shutdown_mgr.register_component("shm_feeds", [this] {
            running_.store(false, std::memory_order_relaxed);
            cv_shutdown_.notify_all();
            // SHM segments are cleaned up by ShmFeedQueue destructor
            logger_->info("SHM feed queues detached");
        }, ShutdownPriority::Network, std::chrono::seconds(2));
    } else {
        shutdown_mgr.register_component("websockets", [this] {
            running_.store(false, std::memory_order_relaxed);
            cv_shutdown_.notify_all();
            upbit_ws_->disconnect();
            binance_ws_->disconnect();
            bithumb_ws_->disconnect();
            mexc_ws_->disconnect();
        }, ShutdownPriority::Network, std::chrono::seconds(5));
    }

    shutdown_mgr.register_component("tcp_server", [this] {
        if (tcp_server_) {
            std::thread([this] { tcp_server_->stop(); }).detach();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }, ShutdownPriority::Network, std::chrono::seconds(1));

    shutdown_mgr.register_component("watchdog", [this] {
        auto& watchdog = watchdog_client();
        watchdog.stop_heartbeat();
        watchdog.disconnect();
    }, ShutdownPriority::Network, std::chrono::seconds(2));

    if (dual_executor_) {
        shutdown_mgr.register_component("order_executor", [this] {
            // running=false is already set by websockets shutdown
            // order_thread is joined in cleanup()
            logger_->info("Order executor: draining queue...");
        }, ShutdownPriority::Order, std::chrono::seconds(10));
    }

    shutdown_mgr.register_component("config_watcher", [this] {
        if (config_watcher_) {
            std::thread([this] { config_watcher_->stop(); }).detach();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }, ShutdownPriority::Strategy, std::chrono::seconds(1));

    if (!engine_mode_) {
        auto& health = health_checker();
        shutdown_mgr.register_component("health_checker", [&health] {
            health.stop();
        }, ShutdownPriority::Storage, std::chrono::seconds(2));

        auto& daily_limit = daily_limiter();
        shutdown_mgr.register_component("daily_limiter", [&daily_limit] {
            daily_limit.stop();
        }, ShutdownPriority::Storage, std::chrono::seconds(2));

        auto& stats = trading_stats();
        shutdown_mgr.register_component("trading_stats", [&stats] {
            stats.save();
            stats.stop();
        }, ShutdownPriority::Storage, std::chrono::seconds(3));

        shutdown_mgr.register_component("alert_service", [] {
            try {
                auto& al = alert_service();
                std::thread([&al] { al.stop(); }).detach();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            } catch (...) {}
        }, ShutdownPriority::Logging, std::chrono::seconds(1));
    }

    if (engine_mode_ && monitor_client_) {
        shutdown_mgr.register_component("monitor_client", [this] {
            monitor_client_->disconnect();
        }, ShutdownPriority::Logging, std::chrono::seconds(1));
    }

    shutdown_mgr.register_component("event_bus", [this] {
        try {
            std::thread([this] { event_bus_->stop(); }).detach();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } catch (...) {}
    }, ShutdownPriority::Logging, std::chrono::seconds(1));

    if (!engine_mode_) {
        shutdown_mgr.register_component("io_context", [this] {
            ioc_.stop();
        }, ShutdownPriority::Logging, std::chrono::seconds(2));
    }
}

// =============================================================================
// Section 10: Symbol Subscription & WebSocket Connect (Standalone only)
// =============================================================================
int Application::subscribe_and_connect() {
    if (!engine_mode_) {
        const auto& primary_symbols = Config::instance().primary_symbols();
        if (primary_symbols.empty()) {
            logger_->error("No primary symbols configured");
            return 1;
        }

        const auto& symbol = primary_symbols[0];
        std::cout << "Symbol: " << symbol.symbol << "\n\n";

        upbit_ws_->subscribe_trade({symbol.upbit});
        binance_ws_->subscribe_trade({symbol.binance});
        bithumb_ws_->subscribe_trade({symbol.bithumb});
        mexc_ws_->subscribe_trade({symbol.mexc});

        std::cout << "Connecting to exchanges...\n";
        upbit_ws_->connect("api.upbit.com", "443", "/websocket/v1");
        std::string binance_symbol_lower = symbol.binance;
        std::transform(binance_symbol_lower.begin(), binance_symbol_lower.end(),
                       binance_symbol_lower.begin(), ::tolower);
        binance_ws_->connect("fstream.binance.com", "443",
                            "/stream?streams=" + binance_symbol_lower + "@aggTrade");
        bithumb_ws_->connect("ws-api.bithumb.com", "443", "/websocket/v1");
        mexc_ws_->connect("contract.mexc.com", "443", "/edge");
    } else {
        std::cout << "SHM mode: waiting for feeder data...\n";
    }

    return 0;
}

// =============================================================================
// Section 11: Thread Start
// =============================================================================
void Application::start_threads() {
    bool has_executor = (dual_executor_ != nullptr);

    // IO Thread (Boost.Asio) — Standalone only
    if (!engine_mode_) {
        io_thread_ = std::thread([this]() { ioc_.run(); });
    }

    // Hot Thread (Core-Pinned, Busy-Poll)
    if (engine_mode_) {
        // SHM mode: consume from 4 SHM queues
        hot_thread_ = std::thread([this, has_executor]() {
            HotThreadShm::Deps deps;
            deps.shm_feeds = shm_feeds_.get();
            deps.calculator = calculator_.get();
            deps.engine = decision_engine_.get();
            deps.order_queue = order_queue_.get();
            deps.event_bus = event_bus_;
            deps.ob_analyzer = ob_analyzer_.get();
            deps.price_upbit = &price_upbit_;
            deps.price_bithumb = &price_bithumb_;
            deps.price_binance = &price_binance_;
            deps.price_mexc = &price_mexc_;
            deps.logger = logger_;
            deps.has_executor = has_executor;
            deps.running = &running_;
            HotThreadShm thread(std::move(deps));
            thread.run();
        });
    } else {
        // Standalone mode: WebSocket -> in-process SPSC Queue
        hot_thread_ = std::thread([this, has_executor]() {
            HotThreadWS::Deps deps;
            deps.ws_queue = ws_queue_.get();
            deps.calculator = calculator_.get();
            deps.engine = decision_engine_.get();
            deps.order_queue = order_queue_.get();
            deps.event_bus = event_bus_;
            deps.price_upbit = &price_upbit_;
            deps.price_bithumb = &price_bithumb_;
            deps.price_binance = &price_binance_;
            deps.price_mexc = &price_mexc_;
            deps.logger = logger_;
            deps.has_executor = has_executor;
            deps.running = &running_;
            HotThreadWS thread(std::move(deps));
            thread.run();
        });
    }

    // Order Thread (Cold — only if executor is available)
    if (has_executor) {
        order_thread_ = std::thread([this]() {
            OrderThread::Deps deps;
            deps.order_queue = order_queue_.get();
            deps.executor = dual_executor_.get();
            deps.engine = decision_engine_.get();
            deps.daily_limit = &daily_limiter();
            deps.stats_tracker = &trading_stats();
            deps.event_bus = event_bus_;
            deps.current_fx_rate = &current_fx_rate_;
            deps.logger = logger_;
            deps.running = &running_;
            OrderThread thread(std::move(deps));
            thread.run();
        });
        logger_->info("Order thread started");
    }

    // Display Thread (Standalone only)
    if (!engine_mode_) {
        display_thread_ = std::thread([this]() {
            DisplayThread::Deps deps;
            deps.calculator = calculator_.get();
            deps.price_upbit = &price_upbit_;
            deps.price_bithumb = &price_bithumb_;
            deps.price_binance = &price_binance_;
            deps.price_mexc = &price_mexc_;
            deps.current_fx_rate = &current_fx_rate_;
            deps.cv_mutex = &cv_mutex_;
            deps.cv_shutdown = &cv_shutdown_;
            deps.running = &running_;
            DisplayThread thread(std::move(deps));
            thread.run();
        });
    }

    // FX Rate Thread (Cold)
    fx_thread_ = std::thread([this]() {
        FxRateThread::Deps deps;
        deps.fx_service = fx_service_.get();
        deps.calculator = calculator_.get();
        deps.current_fx_rate = &current_fx_rate_;
        deps.logger = logger_;
        deps.cv_mutex = &cv_mutex_;
        deps.cv_shutdown = &cv_shutdown_;
        deps.running = &running_;
        FxRateThread thread(std::move(deps));
        thread.run();
    });

    // SystemStarted event
    event_bus_->publish(events::SystemStarted{});

    auto& shutdown_mgr = ShutdownManager::instance();
    logger_->info("System started - {} components registered for shutdown",
                 shutdown_mgr.component_count());
    std::cout << "\nSystem running... Press Ctrl+C to stop.\n\n";
}

// =============================================================================
// Section 12: Wait for Shutdown
// =============================================================================
void Application::wait_for_shutdown() {
    auto& shutdown_mgr = ShutdownManager::instance();
    auto result = shutdown_mgr.wait_for_shutdown(std::chrono::seconds(30));

    shutdown_result_.total_elapsed = result.total_elapsed;
    shutdown_result_.completed_components = result.completed_components;
    shutdown_result_.timeout_components = result.timeout_components;
}

// =============================================================================
// Section 13: Cleanup (Thread Join)
// =============================================================================
void Application::cleanup() {
    running_.store(false, std::memory_order_relaxed);
    cv_shutdown_.notify_all();

    if (hot_thread_.joinable()) hot_thread_.join();
    if (order_thread_.joinable()) order_thread_.join();
    if (fx_thread_.joinable()) fx_thread_.join();
    if (display_thread_.joinable()) display_thread_.join();
    if (io_thread_.joinable()) io_thread_.join();
}

// =============================================================================
// Section 14: Final Stats
// =============================================================================
void Application::print_final_stats() {
    std::cout << "\n=== Shutdown Complete ===\n";
    std::cout << "Elapsed: " << shutdown_result_.total_elapsed.count() << "ms\n";
    std::cout << "Completed: " << shutdown_result_.completed_components.size() << " components\n";

    if (!shutdown_result_.timeout_components.empty()) {
        std::cout << "Timeout: ";
        for (const auto& c : shutdown_result_.timeout_components) std::cout << c << " ";
        std::cout << "\n";
    }

    std::cout << "\nLast prices:\n";
    std::cout << "  Upbit:   " << std::fixed << std::setprecision(0)
              << price_upbit_.load() << " KRW\n";
    std::cout << "  Bithumb: " << price_bithumb_.load() << " KRW\n";
    std::cout << "  Binance: " << std::setprecision(4)
              << price_binance_.load() << " USDT\n";
    std::cout << "  MEXC:    " << price_mexc_.load() << " USDT\n";
    std::cout << "  FX Rate: " << std::setprecision(2)
              << current_fx_rate_.load() << " KRW/USD\n";

    auto best = calculator_->get_best_opportunity();
    if (best) {
        std::cout << "Best premium: " << std::setprecision(2)
                  << best->premium_pct << "% ("
                  << exchange_name(best->buy_exchange) << " -> "
                  << exchange_name(best->sell_exchange) << ")\n";
    }
}

}  // namespace arbitrage
