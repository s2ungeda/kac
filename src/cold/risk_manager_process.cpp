/**
 * TASK_45: Risk Manager Process 구현
 */

#include "arbitrage/cold/risk_manager_process.hpp"
#include "arbitrage/common/config.hpp"

#include <csignal>
#include <iostream>
#include <cstring>
#include <thread>
#include <condition_variable>
#include <mutex>

namespace arbitrage {

static std::atomic<RiskManagerProcess*> g_rm_instance{nullptr};

static void rm_signal_handler(int /*sig*/) {
    auto* rm = g_rm_instance.load(std::memory_order_relaxed);
    if (rm) rm->stop();
}

RiskManagerProcess::RiskManagerProcess(const RiskManagerConfig& config)
    : config_(config)
    , logger_(Logger::get("risk-manager"))
{
}

RiskManagerProcess::~RiskManagerProcess() {
    if (running_.load()) stop();
}

int RiskManagerProcess::run() {
    logger_->info("Risk Manager starting");

    setup_signal_handlers();
    if (config_.config_from_stdin) {
        if (!Config::instance().load_from_stream(std::cin)) {
            logger_->error("Failed to load config from stdin");
            return 1;
        }
    } else {
        Config::instance().load(config_.config_path);
    }

    // DailyLossLimiter 설정
    DailyLimitConfig limit_cfg;
    limit_cfg.daily_loss_limit_krw = config_.daily_loss_limit;
    limit_cfg.warning_threshold_pct = config_.warning_pct * 100.0;  // 0.7 → 70.0
    daily_limiter_.set_config(limit_cfg);

    daily_limiter_.set_kill_switch([this]() {
        stats_.kill_switch_count.fetch_add(1, std::memory_order_relaxed);
        logger_->error("KILL SWITCH: Daily loss limit reached");
    });

    daily_limiter_.on_warning([this](double current_loss, double limit, double pct) {
        logger_->warn("Daily loss warning: {:.0f}/{:.0f} ({:.1f}%)",
            current_loss, limit, pct);
    });

    daily_limiter_.start();

    // UDS 서버 시작 (order-manager 연결 수신)
    server_ = std::make_unique<UnixSocketServer>(config_.risk_socket);
    server_->on_message([this](int /*fd*/, uint8_t type, const void* data, size_t len) {
        if (type == static_cast<uint8_t>(IpcMessageType::OrderResult) &&
            len == sizeof(ShmDualOrderResult)) {
            ShmDualOrderResult result;
            std::memcpy(&result, data, sizeof(result));
            on_order_result(result);
        }
    });
    server_->start();

    running_.store(true, std::memory_order_release);
    logger_->info("Risk Manager running (limit={:.0f} KRW)", config_.daily_loss_limit);

    // 메인 루프 — 시그널 대기
    std::mutex cv_mutex;
    std::condition_variable cv;
    while (running_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(cv_mutex);
        cv.wait_for(lock, std::chrono::seconds(1),
            [this] { return !running_.load(std::memory_order_relaxed); });
    }

    // 종료
    server_->stop();
    daily_limiter_.stop();

    auto daily_stats = daily_limiter_.get_stats();
    logger_->info("Risk Manager stopped: {} results, PnL={:.0f}, kills={}",
        stats_.results_received.load(),
        daily_stats.realized_pnl,
        stats_.kill_switch_count.load());

    return 0;
}

void RiskManagerProcess::stop() {
    running_.store(false, std::memory_order_release);
}

void RiskManagerProcess::setup_signal_handlers() {
    g_rm_instance.store(this, std::memory_order_relaxed);
    std::signal(SIGINT, rm_signal_handler);
    std::signal(SIGTERM, rm_signal_handler);
}

void RiskManagerProcess::on_order_result(const ShmDualOrderResult& result) {
    stats_.results_received.fetch_add(1, std::memory_order_relaxed);

    // 손익 기록
    daily_limiter_.record_trade(result.gross_profit);

    logger_->info("Trade result #{}: profit={:.0f} success={}",
        result.request_id, result.gross_profit,
        result.both_success() ? "yes" : "no");
}

RiskManagerConfig RiskManagerProcess::parse_args(int argc, char* argv[]) {
    RiskManagerConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if ((arg == "--config") && i + 1 < argc) {
            cfg.config_path = argv[++i];
        } else if ((arg == "--socket") && i + 1 < argc) {
            cfg.risk_socket = argv[++i];
        } else if ((arg == "--daily-limit") && i + 1 < argc) {
            cfg.daily_loss_limit = std::stod(argv[++i]);
        } else if ((arg == "--warning-pct") && i + 1 < argc) {
            cfg.warning_pct = std::stod(argv[++i]);
        } else if (arg == "--config-stdin") {
            cfg.config_from_stdin = true;
        } else if (arg == "--verbose" || arg == "-v") {
            cfg.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: risk-manager [options]\n"
                      << "Options:\n"
                      << "  --config PATH       Config file\n"
                      << "  --socket PATH       UDS path (default: " << ipc_paths::RISK_SOCKET << ")\n"
                      << "  --daily-limit KRW   Daily loss limit (default: 100000)\n"
                      << "  --warning-pct PCT   Warning threshold 0.0-1.0 (default: 0.7)\n"
                      << "  --verbose, -v       Verbose logging\n"
                      << "  --help, -h          Show help\n";
            std::exit(0);
        }
    }

    return cfg;
}

}  // namespace arbitrage
