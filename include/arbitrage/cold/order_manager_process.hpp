#pragma once

/**
 * TASK_44: Order Manager Process
 *
 * 독립 프로세스 — SHM Queue에서 주문 수신, 실행, 결과 반환
 *
 * 데이터 흐름:
 *   SHM(request) → DualOrderExecutor.execute_sync() → SHM(result)
 *                                                     → UDS(risk-manager)
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/ipc/order_channel.hpp"
#include "arbitrage/ipc/unix_socket.hpp"
#include "arbitrage/executor/dual_order.hpp"
#include "arbitrage/executor/recovery.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace arbitrage {

// =============================================================================
// OrderManagerConfig
// =============================================================================

struct OrderManagerConfig {
    // SHM
    std::string shm_request{shm_names::ORDERS};
    std::string shm_result{shm_names::ORDER_RESULTS};
    size_t queue_capacity{256};

    // Unix Socket (risk-manager 연결)
    std::string risk_socket{ipc_paths::RISK_SOCKET};

    // 설정
    std::string config_path{"config/config.yaml"};
    bool config_from_stdin{false};
    bool dry_run{false};
    bool verbose{false};
};

// =============================================================================
// OrderManagerStats
// =============================================================================

struct OrderManagerStats {
    std::atomic<uint64_t> orders_received{0};
    std::atomic<uint64_t> orders_executed{0};
    std::atomic<uint64_t> orders_success{0};
    std::atomic<uint64_t> orders_failed{0};
    std::atomic<uint64_t> orders_partial{0};
    std::atomic<int64_t> total_latency_us{0};
    std::chrono::steady_clock::time_point started_at;
};

// =============================================================================
// OrderManagerProcess
// =============================================================================

class OrderManagerProcess {
public:
    explicit OrderManagerProcess(const OrderManagerConfig& config);
    ~OrderManagerProcess();

    OrderManagerProcess(const OrderManagerProcess&) = delete;
    OrderManagerProcess& operator=(const OrderManagerProcess&) = delete;

    // 메인 루프 (블로킹 — SIGTERM으로 종료)
    int run();

    // 종료 요청
    void stop();

    // 상태
    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    const OrderManagerStats& stats() const { return stats_; }

    // CLI 파서
    static OrderManagerConfig parse_args(int argc, char* argv[]);

private:
    void init_order_channel();
    void init_executor();
    void init_risk_client();
    void setup_signal_handlers();

    // 주문 처리
    void process_order(const DualOrderRequest& request);

    // 결과를 risk-manager로 전송
    void send_to_risk_manager(const ShmDualOrderResult& result);

    OrderManagerConfig config_;
    OrderManagerStats stats_;
    std::atomic<bool> running_{false};

    // SHM OrderChannel
    OrderChannel order_channel_;

    // 주문 실행
    std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients_;
    std::shared_ptr<RecoveryManager> recovery_mgr_;
    std::unique_ptr<DualOrderExecutor> executor_;

    // Risk Manager 연결
    std::unique_ptr<UnixSocketClient> risk_client_;

    // Logger
    std::shared_ptr<SimpleLogger> logger_;
};

}  // namespace arbitrage
