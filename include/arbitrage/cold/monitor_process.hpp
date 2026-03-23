#pragma once

/**
 * TASK_46: Monitor Process
 *
 * 모니터링/알림/통계 독립 프로세스
 * - Unix Socket에서 이벤트 수신
 * - AlertService (Telegram/Discord/Slack)
 * - TradingStatsTracker (거래 통계)
 * - TcpServer (CLI 연결)
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/ipc/unix_socket.hpp"
#include "arbitrage/ipc/ipc_protocol.hpp"
#include "arbitrage/ipc/ipc_types.hpp"
#include "arbitrage/ops/alert.hpp"
#include "arbitrage/ops/trading_stats.hpp"
#include "arbitrage/infra/health_check.hpp"
#include "arbitrage/infra/tcp_server.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace arbitrage {

struct MonitorConfig {
    std::string monitor_socket{ipc_paths::MONITOR_SOCKET};
    std::string config_path{"config/config.yaml"};
    int tcp_port{9090};
    bool verbose{false};
};

struct MonitorStats {
    std::atomic<uint64_t> events_received{0};
    std::atomic<uint64_t> alerts_sent{0};
};

class MonitorProcess {
public:
    explicit MonitorProcess(const MonitorConfig& config);
    ~MonitorProcess();

    MonitorProcess(const MonitorProcess&) = delete;
    MonitorProcess& operator=(const MonitorProcess&) = delete;

    int run();
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    const MonitorStats& stats() const { return stats_; }

    static MonitorConfig parse_args(int argc, char* argv[]);

private:
    void setup_signal_handlers();
    void on_ipc_message(uint8_t type, const void* data, size_t len);

    MonitorConfig config_;
    MonitorStats stats_;
    std::atomic<bool> running_{false};

    // UDS 서버 (모든 프로세스 이벤트 수신)
    std::unique_ptr<UnixSocketServer> server_;

    // 모니터링 서비스
    std::unique_ptr<TcpServer> tcp_server_;

    std::shared_ptr<SimpleLogger> logger_;
};

}  // namespace arbitrage
