#pragma once

/**
 * TASK_45: Risk Manager Process
 *
 * 포지션/손익/한도 관리 독립 프로세스
 * - Unix Socket에서 거래 결과 수신 (order-manager)
 * - DailyLossLimiter 실행
 * - 킬스위치 → Engine (Unix Socket)
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/ipc/unix_socket.hpp"
#include "arbitrage/ipc/ipc_protocol.hpp"
#include "arbitrage/ipc/ipc_types.hpp"
#include "arbitrage/ops/daily_limit.hpp"
#include "arbitrage/strategy/risk_model.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace arbitrage {

struct RiskManagerConfig {
    std::string risk_socket{ipc_paths::RISK_SOCKET};
    std::string config_path{"config/config.yaml"};
    bool config_from_stdin{false};
    double daily_loss_limit{100000.0};   // KRW
    double warning_pct{0.7};             // 70% 경고
    bool verbose{false};
};

struct RiskManagerStats {
    std::atomic<uint64_t> results_received{0};
    std::atomic<uint64_t> kill_switch_count{0};
    std::atomic<double> total_pnl{0.0};
};

class RiskManagerProcess {
public:
    explicit RiskManagerProcess(const RiskManagerConfig& config);
    ~RiskManagerProcess();

    RiskManagerProcess(const RiskManagerProcess&) = delete;
    RiskManagerProcess& operator=(const RiskManagerProcess&) = delete;

    int run();
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    const RiskManagerStats& stats() const { return stats_; }

    static RiskManagerConfig parse_args(int argc, char* argv[]);

private:
    void setup_signal_handlers();
    void on_order_result(const ShmDualOrderResult& result);

    RiskManagerConfig config_;
    RiskManagerStats stats_;
    std::atomic<bool> running_{false};

    // UDS 서버 (order-manager 연결 수신)
    std::unique_ptr<UnixSocketServer> server_;

    // 리스크 관리
    DailyLossLimiter daily_limiter_;
    RiskModel risk_model_;

    std::shared_ptr<SimpleLogger> logger_;
};

}  // namespace arbitrage
