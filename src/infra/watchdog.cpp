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
    last_heartbeat_time_ = std::chrono::steady_clock::now();
    window_start_ = std::chrono::steady_clock::now();
    process_start_time_ = std::chrono::system_clock::now();
}

Watchdog::Watchdog(const WatchdogConfig& config)
    : config_(config)
{
    last_heartbeat_time_ = std::chrono::steady_clock::now();
    window_start_ = std::chrono::steady_clock::now();
    process_start_time_ = std::chrono::system_clock::now();
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

    cv_.notify_all();

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
        missed_heartbeat_count_.store(0, std::memory_order_release);
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

    // TODO: 연결된 클라이언트들에게 전송
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
// 하트비트 처리
// =============================================================================

void Watchdog::handle_heartbeat(const Heartbeat& hb) {
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        last_heartbeat_ = hb;
        last_heartbeat_time_ = std::chrono::steady_clock::now();
    }

    missed_heartbeat_count_.store(0, std::memory_order_release);

    // 콜백 호출
    std::vector<HeartbeatCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
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
    auto bus = event_bus_.lock();
    if (bus) {
        events::HeartbeatReceived event;
        event.sequence = hb.sequence;
        event.timestamp = std::chrono::system_clock::now();
        event.component_status = hb.component_status;
        bus->publish(event);
    }
}

Heartbeat Watchdog::last_heartbeat() const {
    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    return last_heartbeat_;
}

int64_t Watchdog::ms_since_last_heartbeat() const {
    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_heartbeat_time_).count();
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
    status.missed_heartbeat_count = missed_heartbeat_count_.load(std::memory_order_relaxed);

    if (status.main_process_running) {
        auto now = std::chrono::system_clock::now();
        status.main_process_uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now - process_start_time_).count();
    }

    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        status.last_heartbeat_data = last_heartbeat_;
        // time_point 변환
        auto elapsed = std::chrono::steady_clock::now() - last_heartbeat_time_;
        status.last_heartbeat = std::chrono::system_clock::now() -
            std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
    }

    {
        std::lock_guard<std::mutex> lock(restart_mutex_);
        if (!restart_history_.empty()) {
            status.last_restart = restart_history_.back().timestamp;
        }
    }

    return status;
}

std::vector<RestartEvent> Watchdog::get_restart_history() const {
    std::lock_guard<std::mutex> lock(restart_mutex_);
    return std::vector<RestartEvent>(restart_history_.begin(), restart_history_.end());
}

// =============================================================================
// 상태 영속화
// =============================================================================

void Watchdog::save_state(const PersistedState& state) {
    auto filename = generate_state_filename();
    auto data = serialize_state(state);

    std::ofstream file(filename, std::ios::binary);
    if (file) {
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
}

std::optional<PersistedState> Watchdog::load_latest_state() {
    auto snapshots = list_state_snapshots(1);
    if (snapshots.empty()) {
        return std::nullopt;
    }

    std::ifstream file(snapshots[0], std::ios::binary);
    if (!file) {
        return std::nullopt;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());

    if (data.empty()) {
        return std::nullopt;
    }

    return deserialize_state(data);
}

std::vector<std::string> Watchdog::list_state_snapshots(int max_count) {
    std::vector<std::string> result;

    try {
        for (const auto& entry : std::filesystem::directory_iterator(config_.state_directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".dat") {
                result.push_back(entry.path().string());
            }
        }
    } catch (...) {
        return result;
    }

    // 최신 순으로 정렬
    std::sort(result.begin(), result.end(), std::greater<>());

    if (static_cast<int>(result.size()) > max_count) {
        result.resize(max_count);
    }

    return result;
}

void Watchdog::cleanup_old_snapshots(int keep_count) {
    auto snapshots = list_state_snapshots(10000);

    for (size_t i = keep_count; i < snapshots.size(); ++i) {
        try {
            std::filesystem::remove(snapshots[i]);
        } catch (const std::exception& e) {
            // 스냅샷 삭제 실패 (무시하고 계속)
        }
    }
}

// =============================================================================
// 콜백 설정
// =============================================================================

void Watchdog::on_restart(RestartCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    restart_callbacks_.push_back(std::move(callback));
}

void Watchdog::on_alert(AlertCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    alert_callbacks_.push_back(std::move(callback));
}

void Watchdog::on_heartbeat(HeartbeatCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    heartbeat_callbacks_.push_back(std::move(callback));
}

void Watchdog::set_event_bus(std::shared_ptr<EventBus> bus) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    event_bus_ = bus;
}

void Watchdog::set_alert_service(std::shared_ptr<AlertService> service) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    alert_service_ = service;
}

// =============================================================================
// 설정
// =============================================================================

void Watchdog::set_config(const WatchdogConfig& config) {
    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    config_ = config;
}

WatchdogConfig Watchdog::config() const {
    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    return config_;
}

// =============================================================================
// 내부 구현
// =============================================================================

void Watchdog::monitor_loop() {
    while (running_.load(std::memory_order_acquire)) {
        // 하트비트 체크
        check_heartbeat();

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
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_for(lock,
                std::chrono::milliseconds(config_.heartbeat_interval_ms),
                [this]() { return !running_.load(std::memory_order_acquire); }
            );
        }
    }
}

void Watchdog::check_heartbeat() {
    int64_t elapsed = ms_since_last_heartbeat();

    if (elapsed > config_.heartbeat_timeout_ms) {
        int missed = missed_heartbeat_count_.fetch_add(1, std::memory_order_relaxed) + 1;

        if (missed >= config_.max_missed_heartbeats) {
            if (config_.restart_on_hang) {
                restart_main_process("Heartbeat timeout (" + std::to_string(elapsed) + "ms)");
            } else {
                send_alert("warning", "Heartbeat timeout: " + std::to_string(elapsed) + "ms");
            }
        }
    }
}

void Watchdog::check_resources() {
    int pid = main_pid_.load(std::memory_order_acquire);
    if (pid <= 0) {
        return;
    }

    ProcessStatus status = get_process_status(pid);

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
        std::lock_guard<std::mutex> lock(restart_mutex_);

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
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
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
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks = alert_callbacks_;
    }

    for (auto& cb : callbacks) {
        try {
            cb(level, message);
        } catch (const std::exception& e) {
            // 알림 콜백 에러 (처리 계속)
        }
    }

    // AlertService 사용
    // auto alert_svc = alert_service_.lock();
    // if (alert_svc) {
    //     // TODO: AlertService 연동
    // }

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

ProcessStatus Watchdog::get_process_status(int pid) const {
    ProcessStatus status;
    status.pid = pid;

#ifndef _WIN32
    // /proc/[pid]/stat 읽기
    std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    std::ifstream stat_file(stat_path);

    if (stat_file) {
        status.is_running = true;

        // 간단히 메모리만 읽기
        std::string statm_path = "/proc/" + std::to_string(pid) + "/statm";
        std::ifstream statm_file(statm_path);

        if (statm_file) {
            size_t size, resident;
            statm_file >> size >> resident;
            status.memory_bytes = resident * 4096;  // 페이지 크기
        }
    } else {
        status.is_running = false;
    }
#endif

    return status;
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

std::string Watchdog::generate_state_filename() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &tm_now);

    static std::atomic<int> counter{0};
    int seq = counter.fetch_add(1, std::memory_order_relaxed) % 1000;

    std::ostringstream oss;
    oss << config_.state_directory << "/state_" << buffer << "_"
        << std::setfill('0') << std::setw(3) << seq << ".dat";

    return oss.str();
}

std::vector<uint8_t> Watchdog::serialize_state(const PersistedState& state) {
    std::vector<uint8_t> data;

    // 간단한 직렬화 (실제로는 MessagePack 또는 Protobuf 사용 권장)

    // 버전 (8바이트)
    for (int i = 0; i < 8; ++i) {
        data.push_back((state.version >> (i * 8)) & 0xFF);
    }

    // 타임스탬프 (8바이트)
    auto ts = std::chrono::duration_cast<std::chrono::microseconds>(
        state.saved_at.time_since_epoch()).count();
    for (int i = 0; i < 8; ++i) {
        data.push_back((ts >> (i * 8)) & 0xFF);
    }

    // total_pnl_today (8바이트)
    uint64_t pnl_bits;
    memcpy(&pnl_bits, &state.total_pnl_today, sizeof(pnl_bits));
    for (int i = 0; i < 8; ++i) {
        data.push_back((pnl_bits >> (i * 8)) & 0xFF);
    }

    // total_trades_today (4바이트)
    for (int i = 0; i < 4; ++i) {
        data.push_back((state.total_trades_today >> (i * 8)) & 0xFF);
    }

    // daily_loss_used (8바이트)
    uint64_t loss_bits;
    memcpy(&loss_bits, &state.daily_loss_used, sizeof(loss_bits));
    for (int i = 0; i < 8; ++i) {
        data.push_back((loss_bits >> (i * 8)) & 0xFF);
    }

    // kill_switch_active (1바이트)
    data.push_back(state.kill_switch_active ? 1 : 0);

    // last_error 길이 + 데이터
    uint32_t error_len = static_cast<uint32_t>(state.last_error.size());
    for (int i = 0; i < 4; ++i) {
        data.push_back((error_len >> (i * 8)) & 0xFF);
    }
    data.insert(data.end(), state.last_error.begin(), state.last_error.end());

    return data;
}

PersistedState Watchdog::deserialize_state(const std::vector<uint8_t>& data) {
    PersistedState state;

    if (data.size() < 37) {  // 최소 크기
        return state;
    }

    size_t offset = 0;

    // 버전
    state.version = 0;
    for (int i = 0; i < 8 && offset < data.size(); ++i, ++offset) {
        state.version |= static_cast<uint64_t>(data[offset]) << (i * 8);
    }

    // 타임스탬프
    uint64_t ts = 0;
    for (int i = 0; i < 8 && offset < data.size(); ++i, ++offset) {
        ts |= static_cast<uint64_t>(data[offset]) << (i * 8);
    }
    state.saved_at = std::chrono::system_clock::time_point(
        std::chrono::microseconds(ts));

    // total_pnl_today
    uint64_t pnl_bits = 0;
    for (int i = 0; i < 8 && offset < data.size(); ++i, ++offset) {
        pnl_bits |= static_cast<uint64_t>(data[offset]) << (i * 8);
    }
    memcpy(&state.total_pnl_today, &pnl_bits, sizeof(state.total_pnl_today));

    // total_trades_today
    state.total_trades_today = 0;
    for (int i = 0; i < 4 && offset < data.size(); ++i, ++offset) {
        state.total_trades_today |= static_cast<int>(data[offset]) << (i * 8);
    }

    // daily_loss_used
    uint64_t loss_bits = 0;
    for (int i = 0; i < 8 && offset < data.size(); ++i, ++offset) {
        loss_bits |= static_cast<uint64_t>(data[offset]) << (i * 8);
    }
    memcpy(&state.daily_loss_used, &loss_bits, sizeof(state.daily_loss_used));

    // kill_switch_active
    if (offset < data.size()) {
        state.kill_switch_active = data[offset++] != 0;
    }

    // last_error
    if (offset + 4 <= data.size()) {
        uint32_t error_len = 0;
        for (int i = 0; i < 4; ++i, ++offset) {
            error_len |= static_cast<uint32_t>(data[offset]) << (i * 8);
        }

        if (offset + error_len <= data.size()) {
            state.last_error.assign(data.begin() + offset, data.begin() + offset + error_len);
        }
    }

    return state;
}

}  // namespace arbitrage
