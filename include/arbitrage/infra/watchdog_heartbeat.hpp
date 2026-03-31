#pragma once

/**
 * Heartbeat Monitor (refactored from watchdog.hpp)
 *
 * Heartbeat protocol, timeout detection, per-process heartbeat tracking
 */

#include "arbitrage/infra/watchdog_client.hpp"
#include "arbitrage/common/spin_wait.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace arbitrage {

// Forward declarations
class EventBus;

// =============================================================================
// HeartbeatMonitor
// =============================================================================

/**
 * 하트비트 모니터
 *
 * 하트비트 수신, 타임아웃 감지, 콜백 호출
 */
class HeartbeatMonitor {
public:
    using HeartbeatCallback = std::function<void(const Heartbeat& hb)>;

    HeartbeatMonitor();

    // =========================================================================
    // 하트비트 처리
    // =========================================================================

    /**
     * 하트비트 처리 (외부에서 호출)
     */
    void handle_heartbeat(const Heartbeat& hb,
                          std::weak_ptr<EventBus> event_bus);

    /**
     * 마지막 하트비트 정보
     */
    Heartbeat last_heartbeat() const;

    /**
     * 마지막 하트비트 이후 경과 시간 (ms)
     */
    int64_t ms_since_last_heartbeat() const;

    // =========================================================================
    // 타임아웃 체크
    // =========================================================================

    /**
     * 하트비트 타임아웃 체크
     * @return missed heartbeat count (after increment), or 0 if no timeout
     */
    int check_timeout(int timeout_ms);

    /**
     * 누락된 하트비트 횟수
     */
    int missed_heartbeat_count() const {
        return missed_heartbeat_count_.load(std::memory_order_relaxed);
    }

    /**
     * missed heartbeat count 리셋
     */
    void reset_missed_count() {
        missed_heartbeat_count_.store(0, std::memory_order_release);
    }

    // =========================================================================
    // 콜백
    // =========================================================================

    void on_heartbeat(HeartbeatCallback callback);

    // =========================================================================
    // 하트비트 시간 정보 (상태 조회용)
    // =========================================================================

    std::chrono::steady_clock::time_point last_heartbeat_steady_time() const;

private:
    mutable RWSpinLock heartbeat_mutex_;
    Heartbeat last_heartbeat_;
    std::chrono::steady_clock::time_point last_heartbeat_time_;

    std::atomic<int> missed_heartbeat_count_{0};

    SpinLock callbacks_mutex_;
    std::vector<HeartbeatCallback> heartbeat_callbacks_;
};

}  // namespace arbitrage
