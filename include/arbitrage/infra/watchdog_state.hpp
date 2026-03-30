#pragma once

/**
 * Watchdog State (refactored from watchdog.hpp)
 *
 * State serialization/deserialization, save_to_file/load_from_file,
 * snapshot data structures (PersistedState, ProcessStatus)
 */

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arbitrage {

// =============================================================================
// 프로세스 상태
// =============================================================================

/**
 * 프로세스 상태
 */
struct ProcessStatus {
    int pid{-1};
    bool is_running{false};
    uint64_t memory_bytes{0};
    double cpu_percent{0.0};
    uint64_t uptime_sec{0};
    int exit_code{0};
    std::string exit_reason;
    std::chrono::system_clock::time_point start_time;

    ProcessStatus() = default;
};

// =============================================================================
// 상태 영속화
// =============================================================================

/**
 * 저장할 상태 (크래시 복구용)
 */
struct PersistedState {
    uint64_t version{1};
    std::chrono::system_clock::time_point saved_at;

    // 포지션 정보
    struct Position {
        std::string exchange;
        std::string symbol;
        double quantity{0.0};
        double avg_price{0.0};
        std::string side;  // "long" / "short"
    };
    std::vector<Position> open_positions;

    // 대기 중인 주문
    struct PendingOrder {
        std::string order_id;
        std::string exchange;
        std::string symbol;
        double quantity{0.0};
        double price{0.0};
        std::string side;
        std::string type;  // "limit" / "market"
        std::chrono::system_clock::time_point created_at;
    };
    std::vector<PendingOrder> pending_orders;

    // 통계
    double total_pnl_today{0.0};
    int total_trades_today{0};
    double daily_loss_used{0.0};

    // 시스템 상태
    bool kill_switch_active{false};
    std::string last_error;
};

// =============================================================================
// WatchdogState
// =============================================================================

/**
 * 상태 영속화 관리
 *
 * 상태 직렬화/역직렬화, 파일 저장/로드, 스냅샷 관리
 */
class WatchdogState {
public:
    WatchdogState() = default;

    /**
     * 상태 디렉토리 설정
     */
    void set_state_directory(const std::string& dir) { state_directory_ = dir; }

    /**
     * 현재 상태 디렉토리
     */
    const std::string& state_directory() const { return state_directory_; }

    // =========================================================================
    // 상태 저장/로드
    // =========================================================================

    void save_state(const PersistedState& state);
    std::optional<PersistedState> load_latest_state();

    // =========================================================================
    // 스냅샷 관리
    // =========================================================================

    std::vector<std::string> list_state_snapshots(int max_count = 10);
    void cleanup_old_snapshots(int keep_count = 100);

    // =========================================================================
    // 프로세스 상태 조회
    // =========================================================================

    static ProcessStatus get_process_status(int pid);

    // =========================================================================
    // 직렬화/역직렬화 (public for testing)
    // =========================================================================

    static std::vector<uint8_t> serialize_state(const PersistedState& state);
    static PersistedState deserialize_state(const std::vector<uint8_t>& data);

private:
    std::string generate_state_filename() const;

    std::string state_directory_{"./state"};
};

}  // namespace arbitrage
