#pragma once

/**
 * Child Process Manager (refactored from watchdog.hpp)
 *
 * fork/exec, waitpid, restart logic, restart history tracking
 * - ChildProcessConfig / ChildProcessInfo structs
 * - ChildProcessManager class
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace arbitrage {

// =============================================================================
// 재시작 기록
// =============================================================================

/**
 * 재시작 이벤트
 */
struct RestartEvent {
    std::chrono::system_clock::time_point timestamp;
    int old_pid{0};
    int new_pid{0};
    std::string reason;
    int exit_code{0};
};

// =============================================================================
// 자식 프로세스 설정
// =============================================================================

/**
 * 자식 프로세스 설정
 */
struct ChildProcessConfig {
    std::string name;                       // 프로세스 식별자 (e.g., "upbit-feeder")
    std::string executable;                 // 실행 파일 경로
    std::vector<std::string> arguments;     // 실행 인자
    int restart_delay_ms{2000};             // 재시작 전 대기 (ms)
    int max_restarts{10};                   // 최대 재시작 횟수 (0 = 무제한)
    int restart_window_sec{3600};           // 재시작 횟수 카운트 윈도우
    bool critical{true};                    // true: 영구 실패 시 전체 시스템 종료
    int start_order{0};                     // 시작 순서 (낮은 번호 먼저)
    int start_delay_ms{0};                  // 이전 프로세스 시작 후 대기 시간
};

/**
 * 자식 프로세스 런타임 정보
 */
struct ChildProcessInfo {
    ChildProcessConfig config;
    int pid{-1};
    bool is_running{false};
    int restart_count{0};
    int exit_code{0};
    std::chrono::system_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_restart_time;
    std::string last_error;
};

// =============================================================================
// ChildProcessManager
// =============================================================================

/**
 * 자식 프로세스 관리자
 *
 * fork/exec, waitpid, restart logic, restart history tracking
 */
class ChildProcessManager {
public:
    using AlertFunc = std::function<void(const std::string& level, const std::string& message)>;
    using ShutdownFunc = std::function<void()>;

    ChildProcessManager() = default;

    /**
     * 알림 함수 설정
     */
    void set_alert_func(AlertFunc func) { alert_func_ = std::move(func); }

    /**
     * 시스템 종료 트리거 함수 설정
     */
    void set_shutdown_func(ShutdownFunc func) { shutdown_func_ = std::move(func); }

    /**
     * 작업 디렉토리 설정
     */
    void set_working_directory(const std::string& dir) { working_directory_ = dir; }

    // =========================================================================
    // 자식 프로세스 등록/해제
    // =========================================================================

    void add_child(const ChildProcessConfig& config);
    void remove_child(const std::string& name);

    // =========================================================================
    // 시작/중지
    // =========================================================================

    void launch_all_children();
    void stop_all_children();
    void restart_child(const std::string& name, const std::string& reason = "manual");

    // =========================================================================
    // 모니터링
    // =========================================================================

    /**
     * 자식 프로세스 상태 체크 (monitor_loop에서 호출)
     * @param system_running 시스템 실행 중 여부 (check 중 종료 트리거에 사용)
     */
    void check_children(std::atomic<bool>& system_running,
                        std::condition_variable& cv);

    // =========================================================================
    // 조회
    // =========================================================================

    std::vector<ChildProcessInfo> get_children_status() const;

    // =========================================================================
    // 기본 구성 생성
    // =========================================================================

    static std::vector<ChildProcessConfig> make_default_children(
        const std::string& bin_dir = "./",
        const std::vector<std::string>& engine_args = {});

    // =========================================================================
    // 프로세스 시작/종료 유틸리티
    // =========================================================================

    int launch_child(const ChildProcessConfig& config);
    void kill_child(int pid, int timeout_ms = 5000);
    bool can_restart_child(const ChildProcessInfo& info) const;

private:
    void send_alert(const std::string& level, const std::string& message);

    mutable std::mutex children_mutex_;
    std::map<std::string, ChildProcessInfo> children_;
    std::string working_directory_;

    AlertFunc alert_func_;
    ShutdownFunc shutdown_func_;
};

}  // namespace arbitrage
