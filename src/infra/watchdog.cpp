#include "arbitrage/infra/watchdog.hpp"
#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#endif

namespace arbitrage {

// =============================================================================
// 싱글톤
// =============================================================================

Watchdog& Watchdog::instance() {
    static Watchdog instance;
    return instance;
}

// =============================================================================
// 생성자/소멸자
// =============================================================================

Watchdog::Watchdog() {
    window_start_ = std::chrono::steady_clock::now();
    process_start_time_ = std::chrono::system_clock::now();

    // Wire up child manager callbacks
    child_manager_.set_alert_func([this](const std::string& level, const std::string& message) {
        send_alert(level, message);
    });
    child_manager_.set_shutdown_func([this]() {
        running_.store(false, std::memory_order_release);
        wakeup_.store(true, std::memory_order_release);
    });
    child_manager_.set_working_directory(config_.working_directory);
    state_manager_.set_state_directory(config_.state_directory);
}

Watchdog::Watchdog(const WatchdogConfig& config)
    : config_(config)
{
    window_start_ = std::chrono::steady_clock::now();
    process_start_time_ = std::chrono::system_clock::now();

    // Wire up child manager callbacks
    child_manager_.set_alert_func([this](const std::string& level, const std::string& message) {
        send_alert(level, message);
    });
    child_manager_.set_shutdown_func([this]() {
        running_.store(false, std::memory_order_release);
        wakeup_.store(true, std::memory_order_release);
    });
    child_manager_.set_working_directory(config_.working_directory);
    state_manager_.set_state_directory(config_.state_directory);
}

Watchdog::~Watchdog() {
    stop();
}

// =============================================================================
// 서비스 제어
// =============================================================================

void Watchdog::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;  // 이미 실행 중
    }

    // 상태 디렉토리 생성
    std::filesystem::create_directories(config_.state_directory);

    // IPC 서버 시작
    start_ipc_server();

    // 모니터 스레드 시작
    monitor_thread_ = std::thread(&Watchdog::monitor_loop, this);
}

void Watchdog::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    wakeup_.store(true, std::memory_order_release);

    // 자식 프로세스 종료 (TASK_40)
    child_manager_.stop_all_children();

    // IPC 서버 중지
    stop_ipc_server();

    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    if (ipc_thread_.joinable()) {
        ipc_thread_.join();
    }
}

// =============================================================================
// 프로세스 제어
// =============================================================================

int Watchdog::launch_main_process() {
    int pid = do_launch();
    if (pid > 0) {
        main_pid_.store(pid, std::memory_order_release);
        process_start_time_ = std::chrono::system_clock::now();
        heartbeat_monitor_.reset_missed_count();
    }
    return pid;
}

void Watchdog::restart_main_process(const std::string& reason) {
    if (!can_restart()) {
        send_alert("error", "Restart limit exceeded: " + reason);
        return;
    }

    do_restart(reason);
}

void Watchdog::request_shutdown() {
    send_command(WatchdogCommand::Shutdown);
}

void Watchdog::force_kill() {
#ifndef _WIN32
    int pid = main_pid_.load(std::memory_order_acquire);
    if (pid > 0) {
        kill(pid, SIGKILL);
        main_pid_.store(-1, std::memory_order_release);
    }
#endif
}

bool Watchdog::is_main_process_running() const {
#ifndef _WIN32
    int pid = main_pid_.load(std::memory_order_acquire);
    if (pid <= 0) {
        return false;
    }
    return kill(pid, 0) == 0;
#else
    return false;
#endif
}

// =============================================================================
// 명령 전송
// =============================================================================

void Watchdog::send_command(WatchdogCommand cmd, const std::string& payload) {
    // 연결된 클라이언트에 명령 전송
    // 간단 구현: 소켓을 통해 전송
#ifndef _WIN32
    if (ipc_socket_fd_ < 0) {
        return;
    }

    std::vector<uint8_t> data;
    data.push_back(static_cast<uint8_t>(cmd));

    // 타임스탬프
    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (int i = 0; i < 8; ++i) {
        data.push_back((ts >> (i * 8)) & 0xFF);
    }

    // 페이로드 길이
    uint32_t payload_len = static_cast<uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i) {
        data.push_back((payload_len >> (i * 8)) & 0xFF);
    }

    // 페이로드
    data.insert(data.end(), payload.begin(), payload.end());

    // IPC 클라이언트 브로드캐스트는 TASK_40에서 구현
#endif
}

void Watchdog::trigger_state_save() {
    send_command(WatchdogCommand::SaveState);
}

void Watchdog::activate_kill_switch(const std::string& reason) {
    send_command(WatchdogCommand::KillSwitch, reason);
    send_alert("critical", "Kill switch activated: " + reason);

    auto bus = event_bus_.lock();
    if (bus) {
        events::KillSwitchActivated event;
        event.reason = reason;
        event.manual = false;
        bus->publish(event);
    }
}

// =============================================================================
// 하트비트 처리 — delegates to HeartbeatMonitor
// =============================================================================

void Watchdog::handle_heartbeat(const Heartbeat& hb) {
    heartbeat_monitor_.handle_heartbeat(hb, event_bus_);
}

Heartbeat Watchdog::last_heartbeat() const {
    return heartbeat_monitor_.last_heartbeat();
}

int64_t Watchdog::ms_since_last_heartbeat() const {
    return heartbeat_monitor_.ms_since_last_heartbeat();
}

// =============================================================================
// 상태 조회
// =============================================================================

Watchdog::Status Watchdog::get_status() const {
    Status status;
    status.running = running_.load(std::memory_order_acquire);
    status.main_process_pid = main_pid_.load(std::memory_order_acquire);
    status.main_process_running = is_main_process_running();
    status.restart_count = restart_count_.load(std::memory_order_relaxed);
    status.missed_heartbeat_count = heartbeat_monitor_.missed_heartbeat_count();

    if (status.main_process_running) {
        auto now = std::chrono::system_clock::now();
        status.main_process_uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now - process_start_time_).count();
    }

    {
        status.last_heartbeat_data = heartbeat_monitor_.last_heartbeat();
        // time_point 변환
        auto elapsed = std::chrono::steady_clock::now() - heartbeat_monitor_.last_heartbeat_steady_time();
        status.last_heartbeat = std::chrono::system_clock::now() -
            std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
    }

    {
        SpinLockGuard lock(restart_mutex_);
        if (!restart_history_.empty()) {
            status.last_restart = restart_history_.back().timestamp;
        }
    }

    return status;
}

std::vector<RestartEvent> Watchdog::get_restart_history() const {
    SpinLockGuard lock(restart_mutex_);
    return std::vector<RestartEvent>(restart_history_.begin(), restart_history_.end());
}

// =============================================================================
// 상태 영속화 — delegates to WatchdogState
// =============================================================================

void Watchdog::save_state(const PersistedState& state) {
    state_manager_.save_state(state);
}

std::optional<PersistedState> Watchdog::load_latest_state() {
    return state_manager_.load_latest_state();
}

std::vector<std::string> Watchdog::list_state_snapshots(int max_count) {
    return state_manager_.list_state_snapshots(max_count);
}

void Watchdog::cleanup_old_snapshots(int keep_count) {
    state_manager_.cleanup_old_snapshots(keep_count);
}

// =============================================================================
// 콜백 설정
// =============================================================================

void Watchdog::on_restart(RestartCallback callback) {
    SpinLockGuard lock(callbacks_mutex_);
    restart_callbacks_.push_back(std::move(callback));
}

void Watchdog::on_alert(AlertCallback callback) {
    SpinLockGuard lock(callbacks_mutex_);
    alert_callbacks_.push_back(std::move(callback));
}

void Watchdog::on_heartbeat(HeartbeatCallback callback) {
    heartbeat_monitor_.on_heartbeat(std::move(callback));
}

void Watchdog::set_event_bus(std::shared_ptr<EventBus> bus) {
    SpinLockGuard lock(callbacks_mutex_);
    event_bus_ = bus;
}

void Watchdog::set_alert_service(std::shared_ptr<AlertService> service) {
    SpinLockGuard lock(callbacks_mutex_);
    alert_service_ = service;
}

// =============================================================================
// 설정
// =============================================================================

void Watchdog::set_config(const WatchdogConfig& config) {
    config_ = config;
    child_manager_.set_working_directory(config_.working_directory);
    state_manager_.set_state_directory(config_.state_directory);
}

WatchdogConfig Watchdog::config() const {
    return config_;
}

// =============================================================================
// 내부 구현
// =============================================================================

void Watchdog::monitor_loop() {
    while (running_.load(std::memory_order_acquire)) {
        // 하트비트 체크
        check_heartbeat();

        // 자식 프로세스 체크 (TASK_40)
        check_children();

        // 리소스 체크 (더 긴 주기)
        static int resource_check_counter = 0;
        if (++resource_check_counter >= (config_.resource_check_interval_ms / config_.heartbeat_interval_ms)) {
            check_resources();
            resource_check_counter = 0;
        }

        // 프로세스 상태 체크
        if (!is_main_process_running() && main_pid_.load(std::memory_order_acquire) > 0) {
            // 프로세스가 종료됨
            int status = 0;
#ifndef _WIN32
            int pid = main_pid_.load(std::memory_order_acquire);
            waitpid(pid, &status, WNOHANG);
#endif
            std::string reason = "Process exited unexpectedly";
            if (WIFEXITED(status)) {
                reason = "Process exited with code " + std::to_string(WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                reason = "Process killed by signal " + std::to_string(WTERMSIG(status));
            }

            main_pid_.store(-1, std::memory_order_release);

            if (config_.restart_on_crash) {
                restart_main_process(reason);
            } else {
                send_alert("critical", reason);
            }
        }

        // 대기
        wakeup_.store(false, std::memory_order_release);
        SpinWait::until_for(
            [this]() {
                return wakeup_.load(std::memory_order_acquire) ||
                       !running_.load(std::memory_order_acquire);
            },
            std::chrono::milliseconds(config_.heartbeat_interval_ms));
    }
}

void Watchdog::check_heartbeat() {
    int missed = heartbeat_monitor_.check_timeout(config_.heartbeat_timeout_ms);

    if (missed > 0 && missed >= config_.max_missed_heartbeats) {
        if (config_.restart_on_hang) {
            int64_t elapsed = heartbeat_monitor_.ms_since_last_heartbeat();
            restart_main_process("Heartbeat timeout (" + std::to_string(elapsed) + "ms)");
        } else {
            int64_t elapsed = heartbeat_monitor_.ms_since_last_heartbeat();
            send_alert("warning", "Heartbeat timeout: " + std::to_string(elapsed) + "ms");
        }
    }
}

void Watchdog::check_resources() {
    int pid = main_pid_.load(std::memory_order_acquire);
    if (pid <= 0) {
        return;
    }

    ProcessStatus status = WatchdogState::get_process_status(pid);

    // 메모리 체크
    if (status.memory_bytes > config_.max_memory_bytes) {
        send_alert("warning", "Memory limit exceeded: " +
            std::to_string(status.memory_bytes / (1024 * 1024)) + "MB");

        if (config_.alert_on_resource_limit) {
            // 경고만 (재시작은 선택적)
        }
    }

    // CPU 체크
    if (status.cpu_percent > config_.max_cpu_percent) {
        send_alert("warning", "CPU limit exceeded: " +
            std::to_string(static_cast<int>(status.cpu_percent)) + "%");
    }
}

bool Watchdog::wait_for_exit(int timeout_ms) {
#ifndef _WIN32
    int pid = main_pid_.load(std::memory_order_acquire);
    if (pid <= 0) {
        return true;
    }

    auto start = std::chrono::steady_clock::now();
    while (true) {
        int status;
        int result = waitpid(pid, &status, WNOHANG);

        if (result == pid) {
            return true;  // 종료됨
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeout_ms) {
            return false;  // 타임아웃
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
    return false;
}

void Watchdog::do_restart(const std::string& reason) {
    int old_pid = main_pid_.load(std::memory_order_acquire);

    // 정상 종료 요청
    if (old_pid > 0) {
        request_shutdown();

        // 종료 대기
        if (!wait_for_exit(10000)) {
            // 강제 종료
            force_kill();
        }
    }

    // 재시작 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.restart_delay_ms));

    // 새 프로세스 시작
    int new_pid = launch_main_process();

    // 재시작 기록
    {
        SpinLockGuard lock(restart_mutex_);

        RestartEvent event;
        event.timestamp = std::chrono::system_clock::now();
        event.old_pid = old_pid;
        event.new_pid = new_pid;
        event.reason = reason;

        restart_history_.push_back(event);

        // 최대 100개 유지
        while (restart_history_.size() > 100) {
            restart_history_.pop_front();
        }
    }

    restart_count_.fetch_add(1, std::memory_order_relaxed);

    // 콜백 호출
    std::vector<RestartCallback> callbacks;
    {
        SpinLockGuard lock(callbacks_mutex_);
        callbacks = restart_callbacks_;
    }

    for (auto& cb : callbacks) {
        try {
            cb(old_pid, new_pid, reason);
        } catch (const std::exception& e) {
            // 재시작 콜백 에러 (처리 계속)
        }
    }

    // 알림
    if (config_.alert_on_restart) {
        send_alert("warning", "Process restarted: " + reason);
    }

    // EventBus 이벤트 발행
    auto bus = event_bus_.lock();
    if (bus) {
        events::ProcessRestarted event;
        event.old_pid = old_pid;
        event.new_pid = new_pid;
        event.reason = reason;
        bus->publish(event);
    }
}

bool Watchdog::can_restart() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - window_start_);

    // 윈도우 리셋
    if (elapsed.count() >= config_.restart_window_sec) {
        const_cast<Watchdog*>(this)->window_start_ = now;
        const_cast<Watchdog*>(this)->restart_count_.store(0, std::memory_order_relaxed);
        return true;
    }

    return restart_count_.load(std::memory_order_relaxed) < config_.max_restarts;
}

void Watchdog::send_alert(const std::string& level, const std::string& message) {
    // 콜백 호출
    std::vector<AlertCallback> callbacks;
    {
        SpinLockGuard lock(callbacks_mutex_);
        callbacks = alert_callbacks_;
    }

    for (auto& cb : callbacks) {
        try {
            cb(level, message);
        } catch (const std::exception& e) {
            // 알림 콜백 에러 (처리 계속)
        }
    }

    // EventBus 이벤트 발행
    auto bus = event_bus_.lock();
    if (bus) {
        events::WatchdogAlert event;
        event.level = level;
        event.message = message;
        event.timestamp = std::chrono::system_clock::now();
        bus->publish(event);
    }
}

int Watchdog::do_launch() {
#ifndef _WIN32
    pid_t pid = fork();

    if (pid < 0) {
        return -1;  // fork 실패
    }

    if (pid == 0) {
        // 자식 프로세스
        // 작업 디렉토리 변경
        if (!config_.working_directory.empty()) {
            chdir(config_.working_directory.c_str());
        }

        // 인자 준비
        std::vector<char*> args;
        args.push_back(const_cast<char*>(config_.main_executable.c_str()));
        for (const auto& arg : config_.main_arguments) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);

        // exec
        execvp(config_.main_executable.c_str(), args.data());

        // exec 실패 시
        _exit(127);
    }

    // 부모 프로세스
    return pid;
#else
    return -1;
#endif
}

void Watchdog::start_ipc_server() {
#ifndef _WIN32
    // 기존 소켓 파일 삭제
    unlink(config_.socket_path.c_str());

    ipc_socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ipc_socket_fd_ < 0) {
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, config_.socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(ipc_socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(ipc_socket_fd_);
        ipc_socket_fd_ = -1;
        return;
    }

    if (listen(ipc_socket_fd_, 5) < 0) {
        close(ipc_socket_fd_);
        ipc_socket_fd_ = -1;
        return;
    }

    // 권한 설정
    chmod(config_.socket_path.c_str(), 0660);

    ipc_running_.store(true, std::memory_order_release);
#endif
}

void Watchdog::stop_ipc_server() {
    ipc_running_.store(false, std::memory_order_release);

#ifndef _WIN32
    if (ipc_socket_fd_ >= 0) {
        close(ipc_socket_fd_);
        ipc_socket_fd_ = -1;
    }

    unlink(config_.socket_path.c_str());
#endif
}

// =============================================================================
// TASK_40: 다중 자식 프로세스 관리 — delegates to ChildProcessManager
// =============================================================================

void Watchdog::add_child(const ChildProcessConfig& config) {
    child_manager_.add_child(config);
}

void Watchdog::remove_child(const std::string& name) {
    child_manager_.remove_child(name);
}

void Watchdog::launch_all_children() {
    child_manager_.launch_all_children();
}

void Watchdog::stop_all_children() {
    child_manager_.stop_all_children();
}

void Watchdog::restart_child(const std::string& name, const std::string& reason) {
    child_manager_.restart_child(name, reason);
}

std::vector<ChildProcessInfo> Watchdog::get_children_status() const {
    return child_manager_.get_children_status();
}

void Watchdog::check_children() {
    child_manager_.check_children(running_, wakeup_);
}

std::vector<ChildProcessConfig> Watchdog::make_default_children(
    const std::string& bin_dir,
    const std::vector<std::string>& engine_args)
{
    return ChildProcessManager::make_default_children(bin_dir, engine_args);
}

}  // namespace arbitrage
