/**
 * Kimchi Arbitrage System - Main Entry Point
 *
 * Architecture: Single-process Hot/Cold Thread separation
 * - Hot Thread: WebSocket → SPSC → Premium → Decision → Order Queue (TASK_31)
 * - Cold Threads: Logger, Alert, Stats, Health, Display, FX Rate
 *
 * Phase 8 통합: TASK_30 (Skeleton) + TASK_31 (Hot/Cold Threads)
 */

// === Core ===
#include "arbitrage/common/config.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/common/fee_calculator.hpp"
#include "arbitrage/common/symbol_master.hpp"
#include "arbitrage/common/config_watcher.hpp"
#include "arbitrage/common/lockfree_queue.hpp"
#include "arbitrage/common/spin_wait.hpp"
#include "arbitrage/common/thread_config.hpp"

// === Strategy ===
#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/strategy/decision_engine.hpp"
#include "arbitrage/strategy/risk_model.hpp"
// StrategyExecutor는 TASK_32에서 통합 (StrategyConfig 이름 충돌 해결 필요)
// #include "arbitrage/strategy/strategy_executor.hpp"

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
#include "arbitrage/common/thread_manager.hpp"

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

// === System ===
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <condition_variable>

using namespace arbitrage;

// =============================================================================
// 설정 상수
// =============================================================================
namespace defaults {
    constexpr double FX_RATE_FALLBACK      = 1450.0;
    constexpr double PREMIUM_ALERT_PCT     = 2.0;
    constexpr int    EVENTBUS_WORKERS      = 2;
    constexpr int    DISPLAY_INTERVAL_SEC  = 10;
    constexpr int    MATRIX_INTERVAL_SEC   = 30;
    constexpr int    FX_UPDATE_INTERVAL_SEC = 30;
    constexpr int    STATS_INTERVAL_SEC    = 60;
    constexpr size_t WS_QUEUE_CAPACITY     = 4096;   // Power of 2
    constexpr size_t ORDER_QUEUE_CAPACITY  = 64;     // Power of 2
    constexpr int    HOT_THREAD_CORE       = 1;      // 코어 1 고정
    constexpr double MIN_EVALUATE_PREMIUM  = 0.1;    // 최소 평가 프리미엄 (%)
}

// =============================================================================
// 커맨드라인 옵션
// =============================================================================
struct AppOptions {
    std::string config_path = "config/config.yaml";
    bool dry_run = false;
    bool verbose = false;
};

AppOptions parse_args(int argc, char* argv[]) {
    AppOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dry-run")       opts.dry_run = true;
        else if (arg == "--verbose")  opts.verbose = true;
        else if (arg == "--config" && i + 1 < argc) opts.config_path = argv[++i];
        else if (arg[0] != '-')       opts.config_path = arg;
    }
    return opts;
}

// =============================================================================
// 유틸리티 함수
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
// Hot Thread (Core-Pinned, Busy-Poll)
// =============================================================================
void hot_thread_func(
    SPSCQueue<WebSocketEvent>& ws_queue,
    PremiumCalculator& calculator,
    DecisionEngine& engine,
    SPSCQueue<DualOrderRequest>& order_queue,
    std::shared_ptr<EventBus> event_bus,
    std::atomic<double>& price_upbit,
    std::atomic<double>& price_bithumb,
    std::atomic<double>& price_binance,
    std::atomic<double>& price_mexc,
    std::shared_ptr<SimpleLogger> logger,
    bool has_executor,
    std::atomic<bool>& running
) {
    // 코어 고정 + RealTime 우선순위 적용
    ThreadConfig hot_cfg;
    hot_cfg.name = "hot_thread";
    hot_cfg.core_id = defaults::HOT_THREAD_CORE;
    hot_cfg.priority = ThreadPriority::High;  // RealTime은 root 필요, High로 대체
    auto apply_result = ThreadManager::apply_to_current(hot_cfg);
    if (apply_result) {
        logger->info("Hot thread: core {} pinned, priority High", defaults::HOT_THREAD_CORE);
    } else {
        logger->warn("Hot thread: failed to apply config ({})", apply_result.error().message);
    }

    AdaptiveSpinWait waiter;
    WebSocketEvent event;
    uint64_t tick_count = 0;
    uint64_t eval_count = 0;

    while (running.load(std::memory_order_relaxed)) {
        bool had_work = false;

        // 1. SPSC Queue 드레인 — 모든 대기 이벤트 처리
        while (ws_queue.pop(event)) {
            had_work = true;

            if (event.is_ticker() || event.is_trade()) {
                double price = event.ticker().price;
                Exchange ex = event.exchange;

                // 2. 프리미엄 계산기 갱신
                calculator.update_price(ex, price);

                // Display 스레드용 atomic 갱신
                switch (ex) {
                    case Exchange::Upbit:   price_upbit.store(price, std::memory_order_relaxed); break;
                    case Exchange::Bithumb: price_bithumb.store(price, std::memory_order_relaxed); break;
                    case Exchange::Binance: price_binance.store(price, std::memory_order_relaxed); break;
                    case Exchange::MEXC:    price_mexc.store(price, std::memory_order_relaxed); break;
                    default: break;
                }

                tick_count++;
            }
        }

        // 3. 기회 판단 (시세 업데이트가 있었을 때만)
        if (had_work) {
            auto best = calculator.get_best_opportunity();
            if (best && best->premium_pct > defaults::MIN_EVALUATE_PREMIUM) {
                auto decision = engine.evaluate(*best);
                eval_count++;

                if (decision.should_execute() && has_executor) {
                    // 4. 주문 큐에 push (Order Thread가 소비 — TASK_32)
                    order_queue.push(decision.order_request);

                    // Cold path 이벤트 발행
                    events::OpportunityDetected opp_evt;
                    opp_evt.premium_pct = best->premium_pct;
                    opp_evt.buy_exchange = best->buy_exchange;
                    opp_evt.sell_exchange = best->sell_exchange;
                    opp_evt.recommended_qty = decision.optimal_qty;
                    opp_evt.expected_profit = decision.expected_profit_krw;
                    opp_evt.confidence = decision.confidence;
                    event_bus->publish(opp_evt);

                    logger->info("ORDER QUEUED: {:.2f}% {} -> {} qty={:.1f}",
                        best->premium_pct,
                        exchange_name(best->buy_exchange),
                        exchange_name(best->sell_exchange),
                        decision.optimal_qty);
                }
            }
        }

        // 5. Adaptive spin: spin → yield → microsleep
        if (!had_work) {
            waiter.wait();
        } else {
            waiter.reset();
        }
    }

    logger->info("Hot thread stopped: {} ticks, {} evaluations", tick_count, eval_count);
}

// =============================================================================
// Order Thread (Cold Path — blocking REST API calls)
// =============================================================================
void order_thread_func(
    SPSCQueue<DualOrderRequest>& order_queue,
    DualOrderExecutor& executor,
    DecisionEngine& engine,
    DailyLossLimiter& daily_limit,
    TradingStatsTracker& stats_tracker,
    std::shared_ptr<EventBus> event_bus,
    std::atomic<double>& current_fx_rate,
    std::shared_ptr<SimpleLogger> logger,
    std::atomic<bool>& running
) {
    // 코어 2 고정
    ThreadConfig order_cfg;
    order_cfg.name = "order_thread";
    order_cfg.core_id = 2;
    order_cfg.priority = ThreadPriority::High;
    ThreadManager::apply_to_current(order_cfg);

    AdaptiveSpinWait waiter;
    DualOrderRequest request;
    uint64_t order_count = 0;

    while (running.load(std::memory_order_relaxed)) {
        if (order_queue.pop(request)) {
            waiter.reset();
            order_count++;

            // DualOrderStarted 이벤트
            events::DualOrderStarted start_evt;
            start_evt.buy_exchange = request.buy_order.exchange;
            start_evt.sell_exchange = request.sell_order.exchange;
            start_evt.quantity = request.buy_order.quantity;
            event_bus->publish(start_evt);

            logger->info("Executing dual order #{}: buy {} sell {}",
                order_count,
                exchange_name(request.buy_order.exchange),
                exchange_name(request.sell_order.exchange));

            // 주문 실행 (블로킹 — REST API 호출, 100ms+ 소요)
            auto result = executor.execute_sync(request);

            // 손익 계산
            double fx_rate = current_fx_rate.load(std::memory_order_relaxed);
            double profit = result.gross_profit(fx_rate);

            // 결과 기록
            engine.record_trade_result(profit);
            daily_limit.record_trade(profit);
            stats_tracker.record_trade(profit);

            // DualOrderCompleted 이벤트
            events::DualOrderCompleted complete_evt;
            complete_evt.success = result.both_filled();
            complete_evt.actual_profit = profit;
            event_bus->publish(complete_evt);

            if (result.both_filled()) {
                logger->info("Dual order #{} filled: profit={:.0f} KRW",
                    order_count, profit);
                engine.start_cooldown(engine.config().cooldown_after_trade);
            } else if (result.partial_fill()) {
                logger->warn("Dual order #{} partial fill — recovery needed", order_count);
                // RecoveryManager는 DualOrderExecutor 내부에서 auto_recovery가 처리
                engine.start_cooldown(engine.config().cooldown_after_loss);
            } else {
                logger->error("Dual order #{} failed", order_count);
                engine.start_cooldown(engine.config().cooldown_after_loss);
            }
        } else {
            waiter.wait();
        }
    }

    logger->info("Order thread stopped: {} orders executed", order_count);
}

// =============================================================================
// Display Thread (Cold Path)
// =============================================================================
void display_thread_func(
    PremiumCalculator& calculator,
    std::atomic<double>& price_upbit,
    std::atomic<double>& price_bithumb,
    std::atomic<double>& price_binance,
    std::atomic<double>& price_mexc,
    std::atomic<double>& current_fx_rate,
    std::mutex& cv_mutex,
    std::condition_variable& cv_shutdown,
    std::atomic<bool>& running
) {
    int cycle = 0;
    while (running.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lock(cv_mutex);
            cv_shutdown.wait_for(lock, std::chrono::seconds(1),
                [&] { return !running.load(std::memory_order_relaxed); });
        }
        if (!running.load(std::memory_order_relaxed)) break;

        cycle++;

        // 10초마다: 가격 요약
        if (cycle % defaults::DISPLAY_INTERVAL_SEC == 0) {
            std::cout << "[" << get_timestamp() << "] "
                      << "Upbit: " << std::fixed << std::setprecision(0)
                      << price_upbit.load()
                      << " | Bithumb: " << price_bithumb.load()
                      << " | Binance: " << std::setprecision(4)
                      << price_binance.load()
                      << " | MEXC: " << price_mexc.load()
                      << " | FX: " << std::setprecision(2)
                      << current_fx_rate.load()
                      << "\n";
        }

        // 30초마다: 매트릭스
        if (cycle % defaults::MATRIX_INTERVAL_SEC == 0) {
            print_matrix(calculator.get_matrix());
        }
    }
}

// =============================================================================
// FX Rate Thread (Cold Path)
// =============================================================================
void fx_rate_thread_func(
    FXRateService& fx_service,
    PremiumCalculator& calculator,
    std::atomic<double>& current_fx_rate,
    std::shared_ptr<SimpleLogger>& logger,
    std::mutex& cv_mutex,
    std::condition_variable& cv_shutdown,
    std::atomic<bool>& running
) {
    while (running.load(std::memory_order_relaxed)) {
        auto fx = fx_service.fetch();
        if (fx) {
            double rate = fx.value().rate;
            current_fx_rate.store(rate, std::memory_order_relaxed);
            calculator.update_fx_rate(rate);
            logger->debug("FX rate updated: {:.2f} ({})", rate, fx.value().source);
        }

        std::unique_lock<std::mutex> lock(cv_mutex);
        cv_shutdown.wait_for(lock,
            std::chrono::seconds(defaults::FX_UPDATE_INTERVAL_SEC),
            [&] { return !running.load(std::memory_order_relaxed); });
    }
}

// =============================================================================
// main()
// =============================================================================
int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);

    // =========================================================================
    // 1. 로거 & 설정
    // =========================================================================
    Logger::init("logs");
    auto logger = Logger::create("main");

    std::cout << "==============================================\n";
    std::cout << "   Kimchi Arbitrage System (C++)\n";
    if (opts.dry_run) std::cout << "   *** DRY RUN MODE ***\n";
    std::cout << "==============================================\n\n";

    logger->info("Starting Kimchi Arbitrage System");
    logger->info("Config: {}", opts.config_path);
    if (opts.dry_run) logger->warn("DRY RUN MODE - No real orders will be placed");

    Config::instance().load(opts.config_path);

    // =========================================================================
    // 2. 시스템 인프라
    // =========================================================================

    // Shutdown Manager (시그널 핸들러)
    auto& shutdown_mgr = ShutdownManager::instance();
    shutdown_mgr.install_signal_handlers();

    // EventBus (비동기 이벤트 처리)
    auto event_bus = EventBus::instance();
    event_bus->start_async(defaults::EVENTBUS_WORKERS);
    shutdown_mgr.set_event_bus(event_bus);

    // 스레드 종료 제어
    std::atomic<bool> running{true};
    std::mutex cv_mutex;
    std::condition_variable cv_shutdown;

    // =========================================================================
    // 3. 트레이딩 인프라
    // =========================================================================

    // Symbol Master
    auto& sym_master = symbol_master();
    sym_master.init_xrp_defaults();

    // Fee Calculator
    auto& fee_calc = fee_calculator();

    // Risk Model
    RiskModel risk_model;
    risk_model.set_fee_calculator(&fee_calc);

    // Decision Engine
    StrategyConfig strategy_config;
    DecisionEngine decision_engine(strategy_config);
    decision_engine.set_risk_model(&risk_model);

    // =========================================================================
    // 4. 네트워크 (WebSocket)
    // =========================================================================
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tlsv12_client};
    ssl_ctx.set_default_verify_paths();

    auto upbit_ws   = std::make_shared<UpbitWebSocket>(ioc, ssl_ctx);
    auto binance_ws = std::make_shared<BinanceWebSocket>(ioc, ssl_ctx);
    auto bithumb_ws = std::make_shared<BithumbWebSocket>(ioc, ssl_ctx);
    auto mexc_ws    = std::make_shared<MEXCWebSocket>(ioc, ssl_ctx);

    // =========================================================================
    // 5. 시세 처리
    // =========================================================================
    FXRateService fx_service;
    PremiumCalculator calculator;

    std::atomic<double> price_upbit{0};
    std::atomic<double> price_bithumb{0};
    std::atomic<double> price_binance{0};
    std::atomic<double> price_mexc{0};
    std::atomic<double> current_fx_rate{0};

    // 초기 환율 로드
    {
        auto fx_result = fx_service.fetch();
        double rate = fx_result ? fx_result.value().rate : defaults::FX_RATE_FALLBACK;
        current_fx_rate.store(rate);
        calculator.update_fx_rate(rate);
        std::cout << "FX Rate: " << std::fixed << std::setprecision(2) << rate
                  << " KRW/USD"
                  << (fx_result ? " (" + fx_result.value().source + ")" : " (default)")
                  << "\n";
    }

    // 프리미엄 콜백
    calculator.set_threshold(defaults::PREMIUM_ALERT_PCT);
    calculator.on_premium_changed([&logger](const PremiumInfo& info) {
        logger->warn("PREMIUM ALERT: {:.2f}% ({} -> {})",
                     info.premium_pct,
                     exchange_name(info.buy_exchange),
                     exchange_name(info.sell_exchange));
    });

    // =========================================================================
    // 6. 주문 실행 (API 키 필요 - 없으면 모니터 전용)
    // =========================================================================
    std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients;
    std::shared_ptr<RecoveryManager> recovery_mgr;
    std::unique_ptr<DualOrderExecutor> dual_executor;

    // TODO: API 키가 설정되면 OrderClient 생성
    // auto upbit_order = create_order_client(Exchange::Upbit, api_key, secret);
    // order_clients[Exchange::Upbit] = upbit_order;
    // ...

    if (!order_clients.empty()) {
        recovery_mgr = std::make_shared<RecoveryManager>(order_clients);
        dual_executor = std::make_unique<DualOrderExecutor>(order_clients, recovery_mgr);
        if (opts.dry_run) {
            recovery_mgr->set_dry_run(true);
        }
        logger->info("Order execution enabled ({} exchanges)", order_clients.size());
    } else {
        logger->info("Monitor-only mode (no API keys configured)");
    }

    // =========================================================================
    // 7. Cold 서비스
    // =========================================================================

    // Health Checker
    auto& health = health_checker();
    health.register_check("upbit_ws", [&]() -> ComponentHealth {
        ComponentHealth h;
        h.name = "upbit_ws";
        h.status = upbit_ws->is_connected() ? HealthStatus::Healthy : HealthStatus::Unhealthy;
        return h;
    });
    health.register_check("binance_ws", [&]() -> ComponentHealth {
        ComponentHealth h;
        h.name = "binance_ws";
        h.status = binance_ws->is_connected() ? HealthStatus::Healthy : HealthStatus::Unhealthy;
        return h;
    });
    health.register_check("bithumb_ws", [&]() -> ComponentHealth {
        ComponentHealth h;
        h.name = "bithumb_ws";
        h.status = bithumb_ws->is_connected() ? HealthStatus::Healthy : HealthStatus::Unhealthy;
        return h;
    });
    health.register_check("mexc_ws", [&]() -> ComponentHealth {
        ComponentHealth h;
        h.name = "mexc_ws";
        h.status = mexc_ws->is_connected() ? HealthStatus::Healthy : HealthStatus::Unhealthy;
        return h;
    });
    health.register_check("decision_engine", [&]() -> ComponentHealth {
        ComponentHealth h;
        h.name = "decision_engine";
        h.status = decision_engine.is_kill_switch_active()
            ? HealthStatus::Degraded : HealthStatus::Healthy;
        return h;
    });
    health.set_event_bus(event_bus);
    // on_unhealthy는 AlertService 초기화 후 설정 (아래)

    // Daily Loss Limiter
    auto& daily_limit = daily_limiter();
    daily_limit.set_kill_switch([&]() {
        decision_engine.set_kill_switch(true);
        auto stats = daily_limit.get_stats();
        decision_engine.set_kill_switch_reason("Daily loss limit reached");
        logger->error("KILL SWITCH: Daily loss limit reached ({:.0f} KRW)", stats.realized_pnl);
    });
    daily_limit.set_event_bus(event_bus);
    daily_limit.start();

    // Alert Service
    auto& alerts = alert_service();
    alerts.set_event_bus(event_bus);
    alerts.start();

    // EventBus 구독: 킬스위치 → 알림
    event_bus->subscribe<events::KillSwitchActivated>(
        [&](const events::KillSwitchActivated& e) {
            alerts.critical("Kill Switch", e.reason);
        });

    event_bus->subscribe<events::DailyLossLimitReached>(
        [&](const events::DailyLossLimitReached& e) {
            alerts.warning("Daily Loss Limit",
                "Loss: " + std::to_string(static_cast<int>(e.loss_amount)) +
                " / Limit: " + std::to_string(static_cast<int>(e.limit)));
        });

    // HealthChecker 콜백 (AlertService 초기화 후)
    health.on_unhealthy([&](const ComponentHealth& ch) {
        alerts.error("Health Check", ch.name + " is unhealthy");
        logger->error("UNHEALTHY: {}", ch.name);
    });
    health.start_periodic_check(std::chrono::seconds(30));

    event_bus->subscribe<events::ExchangeDisconnected>(
        [&](const events::ExchangeDisconnected& e) {
            alerts.error("Exchange Disconnected",
                std::string(exchange_name(e.exchange)) + ": " + e.reason);
        });

    // Trading Stats
    auto& stats = trading_stats();
    event_bus->subscribe<events::DualOrderCompleted>(
        [&](const events::DualOrderCompleted& e) {
            stats.record_trade(e.actual_profit);
        });
    stats.start();

    // Config Watcher
    auto config_watcher = std::make_unique<ConfigWatcher>(
        opts.config_path, std::chrono::milliseconds(5000));
    config_watcher->on_reload([&]() {
        logger->info("Config reloaded: {}", opts.config_path);
        Config::instance().load(opts.config_path);
        events::ConfigReloaded evt;
        evt.config_path = opts.config_path;
        event_bus->publish(evt);
    });
    config_watcher->start();

    // TCP Server
    TcpServerConfig tcp_config;
    tcp_config.port = 9090;
    auto tcp_server = std::make_unique<TcpServer>(tcp_config);
    tcp_server->set_event_bus(event_bus);
    auto tcp_result = tcp_server->start();
    if (tcp_result) {
        logger->info("TCP server started on port {}", tcp_config.port);
    } else {
        logger->warn("TCP server failed to start (non-fatal)");
    }

    // Watchdog Client
    auto& watchdog = watchdog_client();
    watchdog.on_command([&](WatchdogCommand cmd, const std::string& /*payload*/) {
        switch (cmd) {
            case WatchdogCommand::Shutdown:
                shutdown_mgr.initiate_shutdown("Watchdog requested shutdown");
                break;
            case WatchdogCommand::KillSwitch:
                decision_engine.set_kill_switch(true);
                decision_engine.set_kill_switch_reason("Watchdog kill switch");
                break;
            case WatchdogCommand::ReloadConfig:
                config_watcher->reload();
                break;
            default:
                break;
        }
    });
    if (watchdog.connect()) {
        watchdog.start_heartbeat();
        logger->info("Watchdog connected");
    } else {
        logger->info("Watchdog not available (standalone mode)");
    }

    // =========================================================================
    // 8. SPSC Queues + WebSocket → Hot Thread Bridge
    // =========================================================================
    SPSCQueue<WebSocketEvent> ws_queue(defaults::WS_QUEUE_CAPACITY);
    SPSCQueue<DualOrderRequest> order_queue(defaults::ORDER_QUEUE_CAPACITY);

    // WebSocket 콜백: IO 스레드에서 SPSC Queue로 push (fire-and-forget)
    // IO 스레드는 단일 스레드(io_context) → SPSC Producer 조건 충족
    upbit_ws->on_event([&ws_queue](const WebSocketEvent& evt) {
        if (evt.is_ticker() || evt.is_trade()) {
            ws_queue.push(evt);
        }
    });

    binance_ws->on_event([&ws_queue](const WebSocketEvent& evt) {
        if (evt.is_ticker() || evt.is_trade()) {
            ws_queue.push(evt);
        }
    });

    bithumb_ws->on_event([&ws_queue](const WebSocketEvent& evt) {
        if (evt.is_ticker() || evt.is_trade()) {
            ws_queue.push(evt);
        }
    });

    mexc_ws->on_event([&ws_queue](const WebSocketEvent& evt) {
        if (evt.is_ticker() || evt.is_trade()) {
            ws_queue.push(evt);
        }
    });

    // =========================================================================
    // 9. ShutdownManager 등록
    // =========================================================================
    shutdown_mgr.register_component("websockets", [&] {
        running.store(false, std::memory_order_relaxed);
        cv_shutdown.notify_all();
        upbit_ws->disconnect();
        binance_ws->disconnect();
        bithumb_ws->disconnect();
        mexc_ws->disconnect();
    }, ShutdownPriority::Network, std::chrono::seconds(5));

    shutdown_mgr.register_component("tcp_server", [&] {
        if (tcp_server) tcp_server->stop();
    }, ShutdownPriority::Network, std::chrono::seconds(3));

    shutdown_mgr.register_component("watchdog", [&] {
        watchdog.stop_heartbeat();
        watchdog.disconnect();
    }, ShutdownPriority::Network, std::chrono::seconds(2));

    if (dual_executor) {
        shutdown_mgr.register_component("order_executor", [&] {
            // running=false는 websockets shutdown에서 이미 설정됨
            // order_thread는 main에서 join
            logger->info("Order executor: draining queue...");
        }, ShutdownPriority::Order, std::chrono::seconds(10));
    }

    shutdown_mgr.register_component("config_watcher", [&] {
        if (config_watcher) config_watcher->stop();
    }, ShutdownPriority::Strategy, std::chrono::seconds(2));

    shutdown_mgr.register_component("health_checker", [&] {
        health.stop();
    }, ShutdownPriority::Storage, std::chrono::seconds(2));

    shutdown_mgr.register_component("daily_limiter", [&] {
        daily_limit.stop();
    }, ShutdownPriority::Storage, std::chrono::seconds(2));

    shutdown_mgr.register_component("trading_stats", [&] {
        stats.save();
        stats.stop();
    }, ShutdownPriority::Storage, std::chrono::seconds(3));

    shutdown_mgr.register_component("alert_service", [&] {
        alerts.stop();
    }, ShutdownPriority::Logging, std::chrono::seconds(3));

    shutdown_mgr.register_component("event_bus", [&] {
        event_bus->stop();
    }, ShutdownPriority::Logging, std::chrono::seconds(3));

    shutdown_mgr.register_component("io_context", [&] {
        ioc.stop();
    }, ShutdownPriority::Logging, std::chrono::seconds(2));

    // =========================================================================
    // 10. 심볼 구독 & WebSocket 연결
    // =========================================================================
    const auto& primary_symbols = Config::instance().primary_symbols();
    if (primary_symbols.empty()) {
        logger->error("No primary symbols configured");
        return 1;
    }

    const auto& symbol = primary_symbols[0];
    std::cout << "Symbol: " << symbol.symbol << "\n\n";

    upbit_ws->subscribe_trade({symbol.upbit});
    binance_ws->subscribe_trade({symbol.binance});
    bithumb_ws->subscribe_trade({symbol.bithumb});
    mexc_ws->subscribe_trade({symbol.mexc});

    std::cout << "Connecting to exchanges...\n";
    upbit_ws->connect("api.upbit.com", "443", "/websocket/v1");
    std::string binance_symbol_lower = symbol.binance;
    std::transform(binance_symbol_lower.begin(), binance_symbol_lower.end(),
                   binance_symbol_lower.begin(), ::tolower);
    binance_ws->connect("fstream.binance.com", "443",
                        "/stream?streams=" + binance_symbol_lower + "@aggTrade");
    bithumb_ws->connect("ws-api.bithumb.com", "443", "/websocket/v1");
    mexc_ws->connect("contract.mexc.com", "443", "/edge");

    // =========================================================================
    // 11. 스레드 시작
    // =========================================================================
    bool has_executor = (dual_executor != nullptr);

    // IO Thread (Boost.Asio)
    std::thread io_thread([&ioc]() { ioc.run(); });

    // Hot Thread (Core-Pinned, Busy-Poll)
    std::thread hot_thread(hot_thread_func,
        std::ref(ws_queue), std::ref(calculator),
        std::ref(decision_engine), std::ref(order_queue),
        event_bus,
        std::ref(price_upbit), std::ref(price_bithumb),
        std::ref(price_binance), std::ref(price_mexc),
        logger, has_executor, std::ref(running));

    // Order Thread (Cold — only if executor is available)
    std::thread order_thread;
    if (has_executor) {
        order_thread = std::thread(order_thread_func,
            std::ref(order_queue), std::ref(*dual_executor),
            std::ref(decision_engine), std::ref(daily_limit),
            std::ref(stats), event_bus,
            std::ref(current_fx_rate), logger, std::ref(running));
        logger->info("Order thread started");
    }

    // Display Thread (Cold)
    std::thread display_thread(display_thread_func,
        std::ref(calculator),
        std::ref(price_upbit), std::ref(price_bithumb),
        std::ref(price_binance), std::ref(price_mexc),
        std::ref(current_fx_rate),
        std::ref(cv_mutex), std::ref(cv_shutdown),
        std::ref(running));

    // FX Rate Thread (Cold)
    std::thread fx_thread(fx_rate_thread_func,
        std::ref(fx_service), std::ref(calculator),
        std::ref(current_fx_rate), std::ref(logger),
        std::ref(cv_mutex), std::ref(cv_shutdown),
        std::ref(running));

    // SystemStarted 이벤트 발행
    event_bus->publish(events::SystemStarted{});

    logger->info("System started - {} components registered for shutdown",
                 shutdown_mgr.component_count());
    std::cout << "\nSystem running... Press Ctrl+C to stop.\n\n";

    // =========================================================================
    // 12. 종료 대기
    // =========================================================================
    auto shutdown_result = shutdown_mgr.wait_for_shutdown(std::chrono::seconds(30));

    // =========================================================================
    // 13. 스레드 정리
    // =========================================================================
    running.store(false, std::memory_order_relaxed);
    cv_shutdown.notify_all();

    if (hot_thread.joinable()) hot_thread.join();
    if (order_thread.joinable()) order_thread.join();
    if (fx_thread.joinable()) fx_thread.join();
    if (display_thread.joinable()) display_thread.join();
    if (io_thread.joinable()) io_thread.join();

    // =========================================================================
    // 14. 최종 통계
    // =========================================================================
    std::cout << "\n=== Shutdown Complete ===\n";
    std::cout << "Elapsed: " << shutdown_result.total_elapsed.count() << "ms\n";
    std::cout << "Completed: " << shutdown_result.completed_components.size() << " components\n";

    if (!shutdown_result.timeout_components.empty()) {
        std::cout << "Timeout: ";
        for (const auto& c : shutdown_result.timeout_components) std::cout << c << " ";
        std::cout << "\n";
    }

    std::cout << "\nLast prices:\n";
    std::cout << "  Upbit:   " << std::fixed << std::setprecision(0)
              << price_upbit.load() << " KRW\n";
    std::cout << "  Bithumb: " << price_bithumb.load() << " KRW\n";
    std::cout << "  Binance: " << std::setprecision(4)
              << price_binance.load() << " USDT\n";
    std::cout << "  MEXC:    " << price_mexc.load() << " USDT\n";
    std::cout << "  FX Rate: " << std::setprecision(2)
              << current_fx_rate.load() << " KRW/USD\n";

    auto best = calculator.get_best_opportunity();
    if (best) {
        std::cout << "Best premium: " << std::setprecision(2)
                  << best->premium_pct << "% ("
                  << exchange_name(best->buy_exchange) << " -> "
                  << exchange_name(best->sell_exchange) << ")\n";
    }

    Logger::shutdown();
    std::cout << "\nGoodbye!\n";
    return 0;
}
