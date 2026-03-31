#include "arbitrage/infra/watchdog_heartbeat.hpp"
#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"

namespace arbitrage {

// =============================================================================
// 생성자
// =============================================================================

HeartbeatMonitor::HeartbeatMonitor() {
    last_heartbeat_time_ = std::chrono::steady_clock::now();
}

// =============================================================================
// 하트비트 처리
// =============================================================================

void HeartbeatMonitor::handle_heartbeat(const Heartbeat& hb,
                                         std::weak_ptr<EventBus> event_bus) {
    {
        WriteGuard lock(heartbeat_mutex_);
        last_heartbeat_ = hb;
        last_heartbeat_time_ = std::chrono::steady_clock::now();
    }

    missed_heartbeat_count_.store(0, std::memory_order_release);

    // 콜백 호출
    std::vector<HeartbeatCallback> callbacks;
    {
        SpinLockGuard lock(callbacks_mutex_);
        callbacks = heartbeat_callbacks_;
    }

    for (auto& cb : callbacks) {
        try {
            cb(hb);
        } catch (const std::exception& e) {
            // 하트비트 콜백 에러 (처리 계속)
        }
    }

    // EventBus 이벤트 발행
    auto bus = event_bus.lock();
    if (bus) {
        events::HeartbeatReceived event;
        event.sequence = hb.sequence;
        event.timestamp = std::chrono::system_clock::now();
        event.component_status = hb.component_status;
        bus->publish(event);
    }
}

Heartbeat HeartbeatMonitor::last_heartbeat() const {
    ReadGuard lock(heartbeat_mutex_);
    return last_heartbeat_;
}

int64_t HeartbeatMonitor::ms_since_last_heartbeat() const {
    ReadGuard lock(heartbeat_mutex_);
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_heartbeat_time_).count();
}

// =============================================================================
// 타임아웃 체크
// =============================================================================

int HeartbeatMonitor::check_timeout(int timeout_ms) {
    int64_t elapsed = ms_since_last_heartbeat();

    if (elapsed > timeout_ms) {
        int missed = missed_heartbeat_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        return missed;
    }

    return 0;
}

// =============================================================================
// 콜백
// =============================================================================

void HeartbeatMonitor::on_heartbeat(HeartbeatCallback callback) {
    SpinLockGuard lock(callbacks_mutex_);
    heartbeat_callbacks_.push_back(std::move(callback));
}

// =============================================================================
// 시간 정보
// =============================================================================

std::chrono::steady_clock::time_point HeartbeatMonitor::last_heartbeat_steady_time() const {
    ReadGuard lock(heartbeat_mutex_);
    return last_heartbeat_time_;
}

}  // namespace arbitrage
