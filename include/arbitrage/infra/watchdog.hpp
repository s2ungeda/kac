#pragma once

/**
 * Watchdog Service (TASK_26 + TASK_40) — Orchestrator
 *
 * 메인 프로세스 감시 및 장애 복구
 * - 하트비트 모니터링
 * - 자동 재시작
 * - 상태 영속화
 * - 리소스 모니터링
 *
 * TASK_40: 다중 프로세스 관리
 * - 4개 Feeder + arb-engine 시작/감시/재시작
 * - 순서 있는 시작 (Feeder → Engine)
 * - 개별 프로세스 자동 재시작
 *
 * Delegates to:
 * - ChildProcessManager  (watchdog_child.hpp)
 * - HeartbeatMonitor      (watchdog_heartbeat.hpp)
 * - WatchdogState         (watchdog_state.hpp)
 */

#include "arbitrage/infra/watchdog_child.hpp"
#include "arbitrage/infra/watchdog_heartbeat.hpp"
#include "arbitrage/infra/watchdog_state.hpp"
#include "arbitrage/infra/watchdog_client.hpp"
#include "arbitrage/common/spin_wait.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace arbitrage {

// Forward declarations
class EventBus;
class AlertService;

// =============================================================================
// 워치독 설정
// =============================================================================

/**
 * 워치독 설정
 */
struct WatchdogConfig {
    // 메인 프로세스 설정
    std::string main_executable{"./arbitrage"};
    std::vector<std::string> main_arguments;
    std::string working_directory{"./"};

    // 하트비트 설정
    int heartbeat_interval_ms{1000};        // 1초
    int heartbeat_timeout_ms{5000};         // 5초 무응답 시 이상
    int max_missed_heartbeats{3};           // 3회 연속 실패 시 재시작

    // 재시작 설정
    int max_restarts{10};                   // 최대 재시작 횟수
    int restart_window_sec{3600};           // 1시간 내 재시작 횟수 제한
    int restart_delay_ms{5000};             // 재시작 전 대기
    bool restart_on_crash{true};
    bool restart_on_hang{true};             // 하트비트 타임아웃 시

    // 리소스 제한
    uint64_t max_memory_bytes{4ULL * 1024 * 1024 * 1024};  // 4GB
    double max_cpu_percent{90.0};
    int resource_check_interval_ms{10000};  // 10초

    // 상태 저장
    std::string state_directory{"./state"};
    int state_save_interval_ms{5000};       // 5초

    // 알림
    bool alert_on_restart{true};
    bool alert_on_resource_limit{true};

    // IPC 설정
    std::string socket_path{WATCHDOG_SOCKET_PATH};
};

// =============================================================================
// Watchdog
// =============================================================================

/**
 * 워치독 서비스
 *
 * 메인 프로세스를 감시하고 장애 시 복구
 * TASK_40: 다중 자식 프로세스 관리 추가
 *
 * Orchestrator: delegates to ChildProcessManager, HeartbeatMonitor, WatchdogState
 */
class Watchdog {
public:
    using RestartCallback = std::function<void(int old_pid, int new_pid, const std::string& reason)>;
    using AlertCallback = std::function<void(const std::string& level, const std::string& message)>;
    using HeartbeatCallback = std::function<void(const Heartbeat& hb)>;

    /**
     * 싱글톤 인스턴스
     */
    static Watchdog& instance();

    Watchdog();
    explicit Watchdog(const WatchdogConfig& config);
    ~Watchdog();

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    // =========================================================================
    // 서비스 제어
    // =========================================================================

    /**
     * 워치독 시작
     */
    void start();

    /**
     * 워치독 중지
     */
    void stop();

    /**
     * 실행 중인지 확인
     */
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    // =========================================================================
    // 프로세스 제어
    // =========================================================================

    /**
     * 메인 프로세스 시작
     */
    int launch_main_process();

    /**
     * 메인 프로세스 재시작
     */
    void restart_main_process(const std::string& reason);

    /**
     * 메인 프로세스 종료 요청
     */
    void request_shutdown();

    /**
     * 메인 프로세스 강제 종료
     */
    void force_kill();

    /**
     * 메인 프로세스 실행 중인지 확인
     */
    bool is_main_process_running() const;

    /**
     * 메인 프로세스 PID
     */
    int main_process_pid() const {
        return main_pid_.load(std::memory_order_acquire);
    }

    // =========================================================================
    // 명령 전송
    // =========================================================================

    /**
     * 명령 전송
     */
    void send_command(WatchdogCommand cmd, const std::string& payload = "");

    /**
     * 상태 저장 요청
     */
    void trigger_state_save();

    /**
     * 킬스위치 활성화
     */
    void activate_kill_switch(const std::string& reason);

    // =========================================================================
    // 하트비트 처리 (수동)
    // =========================================================================

    /**
     * 하트비트 처리 (외부에서 호출)
     */
    void handle_heartbeat(const Heartbeat& hb);

    /**
     * 마지막 하트비트 정보
     */
    Heartbeat last_heartbeat() const;

    /**
     * 마지막 하트비트 이후 경과 시간 (ms)
     */
    int64_t ms_since_last_heartbeat() const;

    // =========================================================================
    // 상태 조회
    // =========================================================================

    /**
     * 현재 상태
     */
    struct Status {
        bool running{false};
        bool main_process_running{false};
        int main_process_pid{-1};
        uint64_t main_process_uptime_sec{0};
        int restart_count{0};
        int missed_heartbeat_count{0};
        std::chrono::system_clock::time_point last_heartbeat;
        std::chrono::system_clock::time_point last_restart;
        Heartbeat last_heartbeat_data;
    };

    Status get_status() const;

    /**
     * 재시작 횟수
     */
    int restart_count() const {
        return restart_count_.load(std::memory_order_relaxed);
    }

    /**
     * 누락된 하트비트 횟수
     */
    int missed_heartbeat_count() const {
        return heartbeat_monitor_.missed_heartbeat_count();
    }

    /**
     * 재시작 히스토리
     */
    std::vector<RestartEvent> get_restart_history() const;

    // =========================================================================
    // 상태 영속화
    // =========================================================================

    /**
     * 상태 저장
     */
    void save_state(const PersistedState& state);

    /**
     * 상태 로드
     */
    std::optional<PersistedState> load_latest_state();

    /**
     * 스냅샷 목록
     */
    std::vector<std::string> list_state_snapshots(int max_count = 10);

    /**
     * 오래된 스냅샷 정리
     */
    void cleanup_old_snapshots(int keep_count = 100);

    // =========================================================================
    // 콜백 설정
    // =========================================================================

    /**
     * 재시작 콜백
     */
    void on_restart(RestartCallback callback);

    /**
     * 알림 콜백
     */
    void on_alert(AlertCallback callback);

    /**
     * 하트비트 수신 콜백
     */
    void on_heartbeat(HeartbeatCallback callback);

    /**
     * EventBus 연결
     */
    void set_event_bus(std::shared_ptr<EventBus> bus);

    /**
     * AlertService 연결
     */
    void set_alert_service(std::shared_ptr<AlertService> service);

    // =========================================================================
    // 설정
    // =========================================================================

    /**
     * 설정 업데이트
     */
    void set_config(const WatchdogConfig& config);

    /**
     * 현재 설정
     */
    WatchdogConfig config() const;

    // =========================================================================
    // TASK_40: 다중 프로세스 관리
    // =========================================================================

    /**
     * 자식 프로세스 등록
     */
    void add_child(const ChildProcessConfig& config);

    /**
     * 등록된 자식 프로세스 제거 (실행 중이면 종료)
     */
    void remove_child(const std::string& name);

    /**
     * 모든 자식 프로세스 시작 (start_order 순서, start_delay 적용)
     */
    void launch_all_children();

    /**
     * 모든 자식 프로세스 종료 (역순)
     */
    void stop_all_children();

    /**
     * 특정 자식 프로세스 재시작
     */
    void restart_child(const std::string& name, const std::string& reason = "manual");

    /**
     * 자식 프로세스 상태 조회
     */
    std::vector<ChildProcessInfo> get_children_status() const;

    /**
     * 자식 프로세스 모니터링 (이미 실행 중인 자식 감시)
     * monitor_loop에서 호출됨
     */
    void check_children();

    /**
     * 기본 Feeder + Engine 구성 생성
     *
     * @param bin_dir 실행 파일 디렉토리 (e.g., "./build/bin")
     * @param engine_args 엔진 추가 인자 (e.g., {"--dry-run"})
     */
    static std::vector<ChildProcessConfig> make_default_children(
        const std::string& bin_dir = "./",
        const std::vector<std::string>& engine_args = {});

private:
    /**
     * 모니터 스레드
     */
    void monitor_loop();

    /**
     * 하트비트 체크
     */
    void check_heartbeat();

    /**
     * 리소스 체크
     */
    void check_resources();

    /**
     * 프로세스 종료 대기
     */
    bool wait_for_exit(int timeout_ms);

    /**
     * 재시작 실행
     */
    void do_restart(const std::string& reason);

    /**
     * 재시작 제한 체크
     */
    bool can_restart() const;

    /**
     * 알림 발송
     */
    void send_alert(const std::string& level, const std::string& message);

    /**
     * 프로세스 시작
     */
    int do_launch();

    /**
     * IPC 서버 시작
     */
    void start_ipc_server();

    /**
     * IPC 서버 중지
     */
    void stop_ipc_server();

private:
    WatchdogConfig config_;

    // Sub-components
    ChildProcessManager child_manager_;
    HeartbeatMonitor heartbeat_monitor_;
    WatchdogState state_manager_;

    // 상태
    std::atomic<bool> running_{false};
    std::atomic<int> main_pid_{-1};
    std::atomic<int> restart_count_{0};

    // 스레드
    std::thread monitor_thread_;
    std::thread ipc_thread_;
    std::atomic<bool> wakeup_{false};

    // 재시작 추적
    mutable SpinLock restart_mutex_;
    std::chrono::steady_clock::time_point window_start_;
    std::deque<RestartEvent> restart_history_;

    // 프로세스 시작 시간
    std::chrono::system_clock::time_point process_start_time_;

    // 콜백
    SpinLock callbacks_mutex_;
    std::vector<RestartCallback> restart_callbacks_;
    std::vector<AlertCallback> alert_callbacks_;
    std::weak_ptr<EventBus> event_bus_;
    std::weak_ptr<AlertService> alert_service_;

    // IPC 서버
    int ipc_socket_fd_{-1};
    std::atomic<bool> ipc_running_{false};
};

// =============================================================================
// 글로벌 접근자
// =============================================================================

/**
 * Watchdog 싱글톤 접근
 */
inline Watchdog& watchdog() {
    return Watchdog::instance();
}

// =============================================================================
// 편의 함수
// =============================================================================

/**
 * 워치독에서 메인 프로세스 감시 시작
 */
inline void start_watchdog(const WatchdogConfig& config = {}) {
    watchdog().set_config(config);
    watchdog().start();
}

/**
 * 워치독 중지
 */
inline void stop_watchdog() {
    watchdog().stop();
}

/**
 * 메인 프로세스 재시작 요청
 */
inline void request_restart(const std::string& reason = "Manual request") {
    watchdog().restart_main_process(reason);
}

}  // namespace arbitrage
