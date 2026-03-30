/**
 * TASK_44: Order Manager Process 구현
 */

#include "arbitrage/cold/order_manager_process.hpp"
#include "arbitrage/common/config.hpp"
#include "arbitrage/common/spin_wait.hpp"
#include "arbitrage/common/thread_config.hpp"
#include "arbitrage/common/thread_manager.hpp"
#include "arbitrage/ipc/ipc_protocol.hpp"

#include <csignal>
#include <iostream>
#include <cstring>
#include <thread>

namespace arbitrage {

// =============================================================================
// 글로벌 시그널 핸들러
// =============================================================================

static std::atomic<OrderManagerProcess*> g_om_instance{nullptr};

static void om_signal_handler(int /*sig*/) {
    auto* om = g_om_instance.load(std::memory_order_relaxed);
    if (om) om->stop();
}

// =============================================================================
// 생성자/소멸자
// =============================================================================

OrderManagerProcess::OrderManagerProcess(const OrderManagerConfig& config)
    : config_(config)
    , logger_(Logger::get("order-manager"))
{
}

OrderManagerProcess::~OrderManagerProcess() {
    if (running_.load()) stop();
    g_om_instance.compare_exchange_strong(
        reinterpret_cast<OrderManagerProcess*&>(
            const_cast<std::atomic<OrderManagerProcess*>&>(g_om_instance)),
        nullptr);
}

// =============================================================================
// run()
// =============================================================================

int OrderManagerProcess::run() {
    logger_->info("Order Manager starting (dry_run={})", config_.dry_run);

    setup_signal_handlers();

    // 설정 로드
    if (config_.config_from_stdin) {
        if (!Config::instance().load_from_stream(std::cin)) {
            logger_->error("Failed to load config from stdin");
            return 1;
        }
    } else {
        Config::instance().load(config_.config_path);
    }

    // 초기화
    try {
        init_order_channel();
    } catch (const std::exception& e) {
        logger_->error("Failed to init order channel: {}", e.what());
        return 1;
    }

    init_executor();
    init_risk_client();

    running_.store(true, std::memory_order_release);
    stats_.started_at = std::chrono::steady_clock::now();

    // 코어 고정
    ThreadConfig thread_cfg;
    thread_cfg.name = "order_manager";
    thread_cfg.core_id = 2;
    thread_cfg.priority = ThreadPriority::High;
    auto result = ThreadManager::apply_to_current(thread_cfg);
    if (result) {
        logger_->info("Core 2 pinned, priority High");
    }

    logger_->info("Order Manager running — waiting for requests");

    // 메인 루프: SHM Queue 폴링
    AdaptiveSpinWait waiter;
    DualOrderRequest request;

    while (running_.load(std::memory_order_relaxed)) {
        if (order_channel_.pop_request(request)) {
            waiter.reset();
            stats_.orders_received.fetch_add(1, std::memory_order_relaxed);
            process_order(request);
        } else {
            waiter.wait();
        }
    }

    // 종료
    logger_->info("Order Manager stopped: {} received, {} executed, {} success",
        stats_.orders_received.load(), stats_.orders_executed.load(),
        stats_.orders_success.load());

    if (risk_client_) {
        risk_client_->disconnect();
    }

    return 0;
}

void OrderManagerProcess::stop() {
    running_.store(false, std::memory_order_release);
}

// =============================================================================
// 초기화
// =============================================================================

void OrderManagerProcess::init_order_channel() {
    order_channel_ = OrderChannel::create_order_manager_side(config_.queue_capacity);

    if (!order_channel_.request_queue_valid()) {
        throw std::runtime_error("Cannot attach to request SHM queue — is arb-engine running?");
    }

    logger_->info("Order channel attached (request={}, result={})",
        config_.shm_request, config_.shm_result);
}

void OrderManagerProcess::init_executor() {
    // API 키가 설정되면 실제 OrderClient 생성
    // 현재는 dry-run / monitor-only 모드

    if (!order_clients_.empty()) {
        recovery_mgr_ = std::make_shared<RecoveryManager>(order_clients_);
        executor_ = std::make_unique<DualOrderExecutor>(order_clients_, recovery_mgr_);
        if (config_.dry_run) {
            recovery_mgr_->set_dry_run(true);
        }
        logger_->info("Executor initialized ({} exchanges)", order_clients_.size());
    } else {
        logger_->info("Monitor-only mode (no API keys configured)");
    }
}

void OrderManagerProcess::init_risk_client() {
    risk_client_ = std::make_unique<UnixSocketClient>();
    if (risk_client_->connect(config_.risk_socket)) {
        risk_client_->start_recv();
        logger_->info("Connected to risk-manager: {}", config_.risk_socket);
    } else {
        logger_->warn("Risk manager not available: {}", config_.risk_socket);
    }
}

void OrderManagerProcess::setup_signal_handlers() {
    g_om_instance.store(this, std::memory_order_relaxed);
    std::signal(SIGINT, om_signal_handler);
    std::signal(SIGTERM, om_signal_handler);
}

// =============================================================================
// 주문 처리
// =============================================================================

void OrderManagerProcess::process_order(const DualOrderRequest& request) {
    logger_->info("Processing order #{}: buy {} sell {} qty={:.1f} premium={:.2f}%",
        request.request_id,
        exchange_name(request.buy_order.exchange),
        exchange_name(request.sell_order.exchange),
        request.buy_order.quantity,
        request.expected_premium);

    auto start = std::chrono::steady_clock::now();

    ShmDualOrderResult shm_result{};
    shm_result.request_id = request.request_id;

    if (executor_) {
        // 실제 주문 실행
        auto result = executor_->execute_sync(request);
        shm_result = to_shm(result);

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        shm_result.total_latency_us = elapsed.count();

        stats_.orders_executed.fetch_add(1, std::memory_order_relaxed);
        stats_.total_latency_us.fetch_add(elapsed.count(), std::memory_order_relaxed);

        if (result.both_filled()) {
            stats_.orders_success.fetch_add(1, std::memory_order_relaxed);
            logger_->info("Order #{} filled: profit={:.0f}",
                request.request_id, shm_result.gross_profit);
        } else if (result.partial_fill()) {
            stats_.orders_partial.fetch_add(1, std::memory_order_relaxed);
            logger_->warn("Order #{} partial fill — recovery needed", request.request_id);
        } else {
            stats_.orders_failed.fetch_add(1, std::memory_order_relaxed);
            logger_->error("Order #{} failed", request.request_id);
        }
    } else {
        // Dry-run / monitor-only: 시뮬레이션 결과
        shm_result.buy_result.success = true;
        shm_result.buy_result.status = OrderStatus::Filled;
        shm_result.buy_result.exchange = request.buy_order.exchange;
        shm_result.buy_result.filled_qty = request.buy_order.quantity;
        shm_result.buy_result.avg_price = request.buy_order.price;

        shm_result.sell_result.success = true;
        shm_result.sell_result.status = OrderStatus::Filled;
        shm_result.sell_result.exchange = request.sell_order.exchange;
        shm_result.sell_result.filled_qty = request.sell_order.quantity;
        shm_result.sell_result.avg_price = request.sell_order.price;

        shm_result.actual_premium = request.expected_premium;

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        shm_result.total_latency_us = elapsed.count();

        stats_.orders_executed.fetch_add(1, std::memory_order_relaxed);
        stats_.orders_success.fetch_add(1, std::memory_order_relaxed);

        logger_->info("Order #{} simulated (dry-run)", request.request_id);
    }

    // 결과를 SHM Queue에 push (Engine feedback)
    if (order_channel_.result_queue_valid()) {
        if (!order_channel_.push_result(shm_result)) {
            logger_->warn("Result queue full — dropping result #{}", request.request_id);
        }
    }

    // Risk Manager에 전송
    send_to_risk_manager(shm_result);
}

void OrderManagerProcess::send_to_risk_manager(const ShmDualOrderResult& result) {
    if (!risk_client_ || !risk_client_->is_connected()) return;

    risk_client_->send(
        static_cast<uint8_t>(IpcMessageType::OrderResult),
        &result, sizeof(result));
}

// =============================================================================
// CLI 파서
// =============================================================================

OrderManagerConfig OrderManagerProcess::parse_args(int argc, char* argv[]) {
    OrderManagerConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if ((arg == "--config") && i + 1 < argc) {
            cfg.config_path = argv[++i];
        } else if ((arg == "--shm-request") && i + 1 < argc) {
            cfg.shm_request = argv[++i];
        } else if ((arg == "--shm-result") && i + 1 < argc) {
            cfg.shm_result = argv[++i];
        } else if ((arg == "--risk-socket") && i + 1 < argc) {
            cfg.risk_socket = argv[++i];
        } else if ((arg == "--capacity") && i + 1 < argc) {
            cfg.queue_capacity = std::stoull(argv[++i]);
        } else if (arg == "--dry-run") {
            cfg.dry_run = true;
        } else if (arg == "--config-stdin") {
            cfg.config_from_stdin = true;
        } else if (arg == "--verbose" || arg == "-v") {
            cfg.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: order-manager [options]\n"
                      << "Options:\n"
                      << "  --config PATH      Config file (default: config/config.yaml)\n"
                      << "  --shm-request NAME SHM request queue (default: " << shm_names::ORDERS << ")\n"
                      << "  --shm-result NAME  SHM result queue (default: " << shm_names::ORDER_RESULTS << ")\n"
                      << "  --risk-socket PATH Risk manager socket (default: " << ipc_paths::RISK_SOCKET << ")\n"
                      << "  --capacity N       Queue capacity (default: 256)\n"
                      << "  --dry-run          Simulate orders\n"
                      << "  --verbose, -v      Verbose logging\n"
                      << "  --help, -h         Show help\n";
            std::exit(0);
        }
    }

    return cfg;
}

}  // namespace arbitrage
