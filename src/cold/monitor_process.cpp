/**
 * TASK_46: Monitor Process 구현
 */

#include "arbitrage/cold/monitor_process.hpp"
#include "arbitrage/common/config.hpp"

#include "arbitrage/common/spin_wait.hpp"

#include <csignal>
#include <iostream>
#include <cstring>
#include <thread>

namespace arbitrage {

static std::atomic<MonitorProcess*> g_mon_instance{nullptr};

static void mon_signal_handler(int /*sig*/) {
    auto* mon = g_mon_instance.load(std::memory_order_relaxed);
    if (mon) mon->stop();
}

MonitorProcess::MonitorProcess(const MonitorConfig& config)
    : config_(config)
    , logger_(Logger::get("monitor"))
{
}

MonitorProcess::~MonitorProcess() {
    if (running_.load()) stop();
}

int MonitorProcess::run() {
    logger_->info("Monitor starting");

    setup_signal_handlers();
    if (config_.config_from_stdin) {
        if (!Config::instance().load_from_stream(std::cin)) {
            logger_->error("Failed to load config from stdin");
            return 1;
        }
    } else {
        Config::instance().load(config_.config_path);
    }

    // UDS 서버 시작 (모든 프로세스 이벤트 수신)
    server_ = std::make_unique<UnixSocketServer>(config_.monitor_socket);
    server_->on_message([this](int /*fd*/, uint8_t type, const void* data, size_t len) {
        on_ipc_message(type, data, len);
    });
    server_->on_client_connected([this](int fd) {
        logger_->info("Process connected: fd={}", fd);
    });
    server_->on_client_disconnected([this](int fd) {
        logger_->warn("Process disconnected: fd={}", fd);
    });
    server_->start();

    // TCP 서버 (CLI 연결)
    TcpServerConfig tcp_cfg;
    tcp_cfg.port = config_.tcp_port;
    tcp_server_ = std::make_unique<TcpServer>(tcp_cfg);
    auto tcp_result = tcp_server_->start();
    if (tcp_result) {
        logger_->info("TCP server started on port {}", config_.tcp_port);
    } else {
        logger_->warn("TCP server failed to start (non-fatal)");
    }

    // Alert/Stats/Health 서비스
    auto& alerts = alert_service();
    alerts.start();

    auto& stats = trading_stats();
    stats.start();

    auto& health = health_checker();
    health.start_periodic_check(std::chrono::seconds(30));

    running_.store(true, std::memory_order_release);
    logger_->info("Monitor running (tcp={}, uds={})", config_.tcp_port, config_.monitor_socket);

    // 메인 루프 — 시그널 대기
    while (running_.load(std::memory_order_relaxed)) {
        SpinWait::until_for(
            [this] { return !running_.load(std::memory_order_relaxed); },
            std::chrono::seconds(1));
    }

    // 종료
    health.stop();
    stats.save();
    stats.stop();
    try { alerts.stop(); } catch (...) {}
    if (tcp_server_) {
        std::thread([this] { tcp_server_->stop(); }).detach();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    server_->stop();

    logger_->info("Monitor stopped: {} events, {} alerts",
        stats_.events_received.load(), stats_.alerts_sent.load());

    return 0;
}

void MonitorProcess::stop() {
    running_.store(false, std::memory_order_release);
}

void MonitorProcess::setup_signal_handlers() {
    g_mon_instance.store(this, std::memory_order_relaxed);
    std::signal(SIGINT, mon_signal_handler);
    std::signal(SIGTERM, mon_signal_handler);
}

void MonitorProcess::on_ipc_message(uint8_t type, const void* data, size_t len) {
    stats_.events_received.fetch_add(1, std::memory_order_relaxed);

    auto msg_type = static_cast<IpcMessageType>(type);

    switch (msg_type) {
        case IpcMessageType::OrderResult: {
            if (len == sizeof(ShmDualOrderResult)) {
                ShmDualOrderResult result;
                std::memcpy(&result, data, sizeof(result));

                trading_stats().record_trade(result.gross_profit);

                if (!result.both_success()) {
                    alert_service().warning("Order",
                        "Partial/failed order #" + std::to_string(result.request_id));
                    stats_.alerts_sent.fetch_add(1, std::memory_order_relaxed);
                }
            }
            break;
        }

        case IpcMessageType::KillSwitch: {
            alert_service().critical("Kill Switch", "Kill switch activated");
            stats_.alerts_sent.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        case IpcMessageType::HealthPing: {
            // Pong 응답 — broadcast로 돌려보냄
            server_->broadcast(static_cast<uint8_t>(IpcMessageType::HealthPong),
                               nullptr, 0);
            break;
        }

        case IpcMessageType::SystemStatus: {
            logger_->debug("System status received ({} bytes)", len);
            break;
        }

        default:
            logger_->debug("Unknown IPC message type: 0x{:02x} ({} bytes)", type, len);
            break;
    }
}

MonitorConfig MonitorProcess::parse_args(int argc, char* argv[]) {
    MonitorConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if ((arg == "--config") && i + 1 < argc) {
            cfg.config_path = argv[++i];
        } else if ((arg == "--socket") && i + 1 < argc) {
            cfg.monitor_socket = argv[++i];
        } else if ((arg == "--tcp-port") && i + 1 < argc) {
            cfg.tcp_port = std::stoi(argv[++i]);
        } else if (arg == "--config-stdin") {
            cfg.config_from_stdin = true;
        } else if (arg == "--verbose" || arg == "-v") {
            cfg.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: monitor [options]\n"
                      << "Options:\n"
                      << "  --config PATH    Config file\n"
                      << "  --socket PATH    UDS path (default: " << ipc_paths::MONITOR_SOCKET << ")\n"
                      << "  --tcp-port PORT  TCP port for CLI (default: 9090)\n"
                      << "  --verbose, -v    Verbose logging\n"
                      << "  --help, -h       Show help\n";
            std::exit(0);
        }
    }

    return cfg;
}

}  // namespace arbitrage
