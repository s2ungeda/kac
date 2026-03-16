#include "arbitrage/infra/watchdog_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#endif

namespace arbitrage {

// =============================================================================
// 싱글톤
// =============================================================================

WatchdogClient& WatchdogClient::instance() {
    static WatchdogClient instance;
    return instance;
}

// =============================================================================
// 생성자/소멸자
// =============================================================================

WatchdogClient::WatchdogClient() {
    last_heartbeat_ = std::chrono::steady_clock::now();
}

WatchdogClient::WatchdogClient(const WatchdogClientConfig& config)
    : config_(config)
{
    last_heartbeat_ = std::chrono::steady_clock::now();
}

WatchdogClient::~WatchdogClient() {
    stop_heartbeat();
    disconnect();
}

// =============================================================================
// 연결
// =============================================================================

bool WatchdogClient::connect() {
    return connect(config_.socket_path);
}

bool WatchdogClient::connect(const std::string& socket_path) {
    if (connected_.load(std::memory_order_acquire)) {
        return true;
    }

    if (do_connect(socket_path)) {
        connected_.store(true, std::memory_order_release);
        standalone_.store(false, std::memory_order_release);
        notify_connection_change(true);
        return true;
    }

    // 연결 실패 = 워치독 없이 독립 실행
    standalone_.store(true, std::memory_order_release);
    return false;
}

void WatchdogClient::disconnect() {
    bool was_connected = connected_.exchange(false, std::memory_order_acq_rel);

#ifndef _WIN32
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
#endif

    if (was_connected) {
        notify_connection_change(false);
    }
}

bool WatchdogClient::do_connect(const std::string& path) {
#ifndef _WIN32
    // Unix Domain Socket 연결
    socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        return false;
    }

    // Non-blocking 설정
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    // 연결 시도
    int result = ::connect(socket_fd_, (struct sockaddr*)&addr, sizeof(addr));
    if (result < 0) {
        if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
            // Non-blocking 연결 중 - poll로 대기
            struct pollfd pfd;
            pfd.fd = socket_fd_;
            pfd.events = POLLOUT;

            int poll_result = poll(&pfd, 1, config_.connect_timeout_ms);
            if (poll_result <= 0 || !(pfd.revents & POLLOUT)) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }

            // 연결 에러 확인
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                close(socket_fd_);
                socket_fd_ = -1;
                return false;
            }
        } else if (errno != EISCONN) {
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
    }

    return true;
#else
    // Windows Named Pipe
    // TODO: Windows 구현
    return false;
#endif
}

// =============================================================================
// 하트비트
// =============================================================================

void WatchdogClient::start_heartbeat() {
    start_heartbeat(config_.heartbeat_interval_ms);
}

void WatchdogClient::start_heartbeat(int interval_ms) {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;  // 이미 실행 중
    }

    config_.heartbeat_interval_ms = interval_ms;

    heartbeat_thread_ = std::thread(&WatchdogClient::heartbeat_loop, this);
}

void WatchdogClient::stop_heartbeat() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    cv_.notify_all();

    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

bool WatchdogClient::send_heartbeat_now() {
    if (!connected_.load(std::memory_order_acquire)) {
        return false;
    }

    Heartbeat hb;
    hb.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    hb.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    hb.active_connections = active_connections_.load(std::memory_order_relaxed);
    hb.pending_orders = pending_orders_.load(std::memory_order_relaxed);
    hb.component_status = component_status_.load(std::memory_order_relaxed);
    hb.error_count = error_count_.load(std::memory_order_relaxed);
    hb.warning_count = warning_count_.load(std::memory_order_relaxed);

    // 메모리/CPU는 필요시 조회
    // TODO: 실제 값 조회

    auto data = serialize_heartbeat(hb);
    if (do_send(data)) {
        heartbeat_count_.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(time_mutex_);
        last_heartbeat_ = std::chrono::steady_clock::now();
        return true;
    }

    return false;
}

void WatchdogClient::heartbeat_loop() {
    while (running_.load(std::memory_order_acquire)) {
        // 하트비트 전송
        if (connected_.load(std::memory_order_acquire)) {
            send_heartbeat_now();
        } else if (config_.auto_reconnect) {
            try_reconnect();
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

// =============================================================================
// 상태 업데이트
// =============================================================================

void WatchdogClient::update_status(
    uint32_t active_connections,
    uint32_t pending_orders,
    uint8_t component_status,
    uint32_t error_count,
    uint32_t warning_count)
{
    active_connections_.store(active_connections, std::memory_order_relaxed);
    pending_orders_.store(pending_orders, std::memory_order_relaxed);
    component_status_.store(component_status, std::memory_order_relaxed);
    error_count_.store(error_count, std::memory_order_relaxed);
    warning_count_.store(warning_count, std::memory_order_relaxed);
}

void WatchdogClient::set_active_connections(uint32_t count) {
    active_connections_.store(count, std::memory_order_relaxed);
}

void WatchdogClient::set_pending_orders(uint32_t count) {
    pending_orders_.store(count, std::memory_order_relaxed);
}

void WatchdogClient::set_component_status(uint8_t status) {
    component_status_.store(status, std::memory_order_relaxed);
}

void WatchdogClient::set_error_count(uint32_t count) {
    error_count_.store(count, std::memory_order_relaxed);
}

void WatchdogClient::set_warning_count(uint32_t count) {
    warning_count_.store(count, std::memory_order_relaxed);
}

void WatchdogClient::set_component_ok(int bit, bool ok) {
    uint8_t current = component_status_.load(std::memory_order_relaxed);
    uint8_t new_value;
    if (ok) {
        new_value = current | (1 << bit);
    } else {
        new_value = current & ~(1 << bit);
    }
    component_status_.store(new_value, std::memory_order_relaxed);
}

Heartbeat WatchdogClient::current_heartbeat() const {
    Heartbeat hb;
    hb.sequence = sequence_.load(std::memory_order_relaxed);
    hb.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    hb.active_connections = active_connections_.load(std::memory_order_relaxed);
    hb.pending_orders = pending_orders_.load(std::memory_order_relaxed);
    hb.component_status = component_status_.load(std::memory_order_relaxed);
    hb.error_count = error_count_.load(std::memory_order_relaxed);
    hb.warning_count = warning_count_.load(std::memory_order_relaxed);
    return hb;
}

// =============================================================================
// 콜백
// =============================================================================

void WatchdogClient::on_command(CommandCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    command_callbacks_.push_back(std::move(callback));
}

void WatchdogClient::on_connection_change(ConnectionCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    connection_callbacks_.push_back(std::move(callback));
}

void WatchdogClient::notify_command(WatchdogCommand cmd, const std::string& payload) {
    std::vector<CommandCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks = command_callbacks_;
    }

    command_count_.fetch_add(1, std::memory_order_relaxed);

    for (auto& cb : callbacks) {
        try {
            cb(cmd, payload);
        } catch (...) {}
    }
}

void WatchdogClient::notify_connection_change(bool connected) {
    std::vector<ConnectionCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks = connection_callbacks_;
    }

    for (auto& cb : callbacks) {
        try {
            cb(connected);
        } catch (...) {}
    }
}

// =============================================================================
// 메시지 전송
// =============================================================================

void WatchdogClient::request_state_save() {
    // 워치독에게 상태 저장 요청 전송
    // 간단한 명령 메시지
    std::vector<uint8_t> data;
    data.push_back(static_cast<uint8_t>(WatchdogCommand::SaveState));
    do_send(data);
}

void WatchdogClient::send_pong() {
    std::vector<uint8_t> data;
    data.push_back(static_cast<uint8_t>(WatchdogCommand::Ping));  // Pong으로 응답
    do_send(data);
}

// =============================================================================
// 통계
// =============================================================================

std::chrono::steady_clock::time_point WatchdogClient::last_heartbeat_time() const {
    std::lock_guard<std::mutex> lock(time_mutex_);
    return last_heartbeat_;
}

// =============================================================================
// 설정
// =============================================================================

void WatchdogClient::set_config(const WatchdogClientConfig& config) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    config_ = config;
}

WatchdogClientConfig WatchdogClient::config() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return config_;
}

// =============================================================================
// 내부 구현
// =============================================================================

bool WatchdogClient::try_reconnect() {
    if (reconnect_count_.load(std::memory_order_relaxed) >= config_.max_reconnect_attempts) {
        return false;
    }

    disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.reconnect_delay_ms));

    if (do_connect(config_.socket_path)) {
        connected_.store(true, std::memory_order_release);
        standalone_.store(false, std::memory_order_release);
        reconnect_count_.fetch_add(1, std::memory_order_relaxed);
        notify_connection_change(true);
        return true;
    }

    return false;
}

bool WatchdogClient::do_send(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return false;
    }

#ifndef _WIN32
    if (socket_fd_ < 0) {
        return false;
    }

    ssize_t sent = send(socket_fd_, data.data(), data.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(data.size());
#else
    return false;
#endif
}

std::vector<uint8_t> WatchdogClient::serialize_heartbeat(const Heartbeat& hb) {
    std::vector<uint8_t> data;
    data.reserve(64);

    // 메시지 타입 (1바이트)
    data.push_back(0x01);  // Heartbeat type

    // 시퀀스 (8바이트)
    for (int i = 0; i < 8; ++i) {
        data.push_back((hb.sequence >> (i * 8)) & 0xFF);
    }

    // 타임스탬프 (8바이트)
    for (int i = 0; i < 8; ++i) {
        data.push_back((hb.timestamp_us >> (i * 8)) & 0xFF);
    }

    // active_connections (4바이트)
    for (int i = 0; i < 4; ++i) {
        data.push_back((hb.active_connections >> (i * 8)) & 0xFF);
    }

    // pending_orders (4바이트)
    for (int i = 0; i < 4; ++i) {
        data.push_back((hb.pending_orders >> (i * 8)) & 0xFF);
    }

    // memory_usage_bytes (8바이트)
    for (int i = 0; i < 8; ++i) {
        data.push_back((hb.memory_usage_bytes >> (i * 8)) & 0xFF);
    }

    // cpu_usage_pct (8바이트 double)
    uint64_t cpu_bits;
    memcpy(&cpu_bits, &hb.cpu_usage_pct, sizeof(cpu_bits));
    for (int i = 0; i < 8; ++i) {
        data.push_back((cpu_bits >> (i * 8)) & 0xFF);
    }

    // component_status (1바이트)
    data.push_back(hb.component_status);

    // error_count (4바이트)
    for (int i = 0; i < 4; ++i) {
        data.push_back((hb.error_count >> (i * 8)) & 0xFF);
    }

    // warning_count (4바이트)
    for (int i = 0; i < 4; ++i) {
        data.push_back((hb.warning_count >> (i * 8)) & 0xFF);
    }

    return data;
}

WatchdogCommandMessage WatchdogClient::deserialize_command(const std::vector<uint8_t>& data) {
    WatchdogCommandMessage msg;

    if (data.empty()) {
        return msg;
    }

    msg.command = static_cast<WatchdogCommand>(data[0]);

    if (data.size() > 1) {
        // 타임스탬프
        msg.timestamp_us = 0;
        for (size_t i = 0; i < 8 && i + 1 < data.size(); ++i) {
            msg.timestamp_us |= static_cast<uint64_t>(data[i + 1]) << (i * 8);
        }
    }

    if (data.size() > 9) {
        // 페이로드 길이
        uint32_t payload_len = 0;
        for (size_t i = 0; i < 4 && i + 9 < data.size(); ++i) {
            payload_len |= static_cast<uint32_t>(data[i + 9]) << (i * 8);
        }

        if (payload_len > 0 && data.size() > 13) {
            msg.payload.assign(data.begin() + 13, data.end());
        }
    }

    return msg;
}

}  // namespace arbitrage
