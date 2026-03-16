#pragma once

/**
 * Watchdog Client (TASK_26)
 *
 * 메인 프로세스에서 워치독과 통신하는 클라이언트
 * - 하트비트 전송
 * - 명령 수신
 * - 상태 업데이트
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace arbitrage {

// =============================================================================
// 하트비트 메시지
// =============================================================================

/**
 * 하트비트 데이터 (메인 → 워치독)
 */
struct Heartbeat {
    uint64_t sequence{0};                   // 시퀀스 번호
    uint64_t timestamp_us{0};               // microseconds since epoch

    // 상태 정보
    uint32_t active_connections{0};         // WebSocket 연결 수
    uint32_t pending_orders{0};             // 대기 중인 주문
    uint64_t memory_usage_bytes{0};         // 메모리 사용량
    double cpu_usage_pct{0.0};              // CPU 사용률

    // 컴포넌트 상태 비트 플래그
    // bit 0: WebSocket OK
    // bit 1: Strategy OK
    // bit 2: Executor OK
    // bit 3: TCP Server OK
    uint8_t component_status{0};

    // 에러 카운트
    uint32_t error_count{0};
    uint32_t warning_count{0};

    /**
     * 컴포넌트 상태 확인
     */
    bool is_healthy() const {
        return (component_status & 0x07) == 0x07;  // WS, Strategy, Executor OK
    }

    /**
     * 특정 컴포넌트 상태 확인
     */
    bool is_component_ok(int bit) const {
        return (component_status & (1 << bit)) != 0;
    }

    /**
     * 컴포넌트 상태 설정
     */
    void set_component(int bit, bool ok) {
        if (ok) {
            component_status |= (1 << bit);
        } else {
            component_status &= ~(1 << bit);
        }
    }
};

// 컴포넌트 비트 상수
namespace ComponentBit {
    constexpr int WebSocket = 0;
    constexpr int Strategy = 1;
    constexpr int Executor = 2;
    constexpr int TcpServer = 3;
    constexpr int OrderManager = 4;
    constexpr int FXRate = 5;
    constexpr int Database = 6;
    constexpr int Alert = 7;
}

// =============================================================================
// 워치독 명령
// =============================================================================

/**
 * 워치독 → 메인 명령
 */
enum class WatchdogCommand : uint8_t {
    None = 0,
    Shutdown = 1,           // 정상 종료 요청
    SaveState = 2,          // 상태 저장 요청
    ReloadConfig = 3,       // 설정 리로드
    KillSwitch = 4,         // 긴급 중단
    HealthCheck = 5,        // 즉시 상태 보고 요청
    Ping = 6,               // 핑 (연결 확인)
    Restart = 7             // 재시작 준비
};

/**
 * 명령 이름
 */
inline const char* watchdog_command_name(WatchdogCommand cmd) {
    switch (cmd) {
        case WatchdogCommand::None:         return "None";
        case WatchdogCommand::Shutdown:     return "Shutdown";
        case WatchdogCommand::SaveState:    return "SaveState";
        case WatchdogCommand::ReloadConfig: return "ReloadConfig";
        case WatchdogCommand::KillSwitch:   return "KillSwitch";
        case WatchdogCommand::HealthCheck:  return "HealthCheck";
        case WatchdogCommand::Ping:         return "Ping";
        case WatchdogCommand::Restart:      return "Restart";
        default: return "Unknown";
    }
}

/**
 * 명령 메시지
 */
struct WatchdogCommandMessage {
    WatchdogCommand command{WatchdogCommand::None};
    uint64_t timestamp_us{0};
    std::string payload;    // JSON 추가 데이터
};

// =============================================================================
// IPC 경로
// =============================================================================

#ifdef _WIN32
constexpr const char* WATCHDOG_SOCKET_PATH = "\\\\.\\pipe\\arbitrage_watchdog";
#else
constexpr const char* WATCHDOG_SOCKET_PATH = "/tmp/arbitrage_watchdog.sock";
#endif

// =============================================================================
// WatchdogClient 설정
// =============================================================================

struct WatchdogClientConfig {
    std::string socket_path{WATCHDOG_SOCKET_PATH};
    int heartbeat_interval_ms{1000};        // 하트비트 주기 (1초)
    int connect_timeout_ms{5000};           // 연결 타임아웃
    int reconnect_delay_ms{3000};           // 재연결 대기
    int max_reconnect_attempts{10};         // 최대 재연결 시도
    bool auto_reconnect{true};              // 자동 재연결
};

// =============================================================================
// WatchdogClient
// =============================================================================

/**
 * 워치독 클라이언트
 *
 * 메인 프로세스에서 워치독에 하트비트를 전송하고 명령을 수신
 */
class WatchdogClient {
public:
    using CommandCallback = std::function<void(WatchdogCommand cmd, const std::string& payload)>;
    using ConnectionCallback = std::function<void(bool connected)>;

    /**
     * 싱글톤 인스턴스
     */
    static WatchdogClient& instance();

    WatchdogClient();
    explicit WatchdogClient(const WatchdogClientConfig& config);
    ~WatchdogClient();

    WatchdogClient(const WatchdogClient&) = delete;
    WatchdogClient& operator=(const WatchdogClient&) = delete;

    // =========================================================================
    // 연결
    // =========================================================================

    /**
     * 워치독에 연결
     * @return 연결 성공 여부
     */
    bool connect();
    bool connect(const std::string& socket_path);

    /**
     * 연결 해제
     */
    void disconnect();

    /**
     * 연결 상태 확인
     */
    bool is_connected() const {
        return connected_.load(std::memory_order_acquire);
    }

    /**
     * 워치독 없이 독립 실행 중인지 확인
     */
    bool is_standalone() const {
        return standalone_.load(std::memory_order_acquire);
    }

    // =========================================================================
    // 하트비트
    // =========================================================================

    /**
     * 하트비트 자동 전송 시작
     */
    void start_heartbeat();
    void start_heartbeat(int interval_ms);

    /**
     * 하트비트 전송 중지
     */
    void stop_heartbeat();

    /**
     * 실행 중인지 확인
     */
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    /**
     * 즉시 하트비트 전송
     */
    bool send_heartbeat_now();

    // =========================================================================
    // 상태 업데이트
    // =========================================================================

    /**
     * 상태 업데이트 (다음 하트비트에 포함)
     */
    void update_status(
        uint32_t active_connections,
        uint32_t pending_orders,
        uint8_t component_status,
        uint32_t error_count = 0,
        uint32_t warning_count = 0
    );

    /**
     * 개별 필드 업데이트
     */
    void set_active_connections(uint32_t count);
    void set_pending_orders(uint32_t count);
    void set_component_status(uint8_t status);
    void set_error_count(uint32_t count);
    void set_warning_count(uint32_t count);

    /**
     * 특정 컴포넌트 상태 설정
     */
    void set_component_ok(int bit, bool ok);

    /**
     * 현재 하트비트 데이터 조회
     */
    Heartbeat current_heartbeat() const;

    // =========================================================================
    // 콜백
    // =========================================================================

    /**
     * 명령 수신 콜백 등록
     */
    void on_command(CommandCallback callback);

    /**
     * 연결 상태 변경 콜백
     */
    void on_connection_change(ConnectionCallback callback);

    // =========================================================================
    // 메시지 전송
    // =========================================================================

    /**
     * 상태 저장 요청 (워치독에게)
     */
    void request_state_save();

    /**
     * Pong 응답
     */
    void send_pong();

    // =========================================================================
    // 통계
    // =========================================================================

    /**
     * 전송한 하트비트 수
     */
    uint64_t heartbeat_count() const {
        return heartbeat_count_.load(std::memory_order_relaxed);
    }

    /**
     * 수신한 명령 수
     */
    uint64_t command_count() const {
        return command_count_.load(std::memory_order_relaxed);
    }

    /**
     * 마지막 하트비트 시간
     */
    std::chrono::steady_clock::time_point last_heartbeat_time() const;

    /**
     * 재연결 횟수
     */
    int reconnect_count() const {
        return reconnect_count_.load(std::memory_order_relaxed);
    }

    // =========================================================================
    // 설정
    // =========================================================================

    /**
     * 설정 업데이트
     */
    void set_config(const WatchdogClientConfig& config);

    /**
     * 현재 설정
     */
    WatchdogClientConfig config() const;

private:
    /**
     * 하트비트 스레드
     */
    void heartbeat_loop();

    /**
     * 수신 스레드
     */
    void receive_loop();

    /**
     * 재연결 시도
     */
    bool try_reconnect();

    /**
     * 실제 연결 처리
     */
    bool do_connect(const std::string& path);

    /**
     * 실제 전송 처리
     */
    bool do_send(const std::vector<uint8_t>& data);

    /**
     * 하트비트 직렬화
     */
    std::vector<uint8_t> serialize_heartbeat(const Heartbeat& hb);

    /**
     * 명령 역직렬화
     */
    WatchdogCommandMessage deserialize_command(const std::vector<uint8_t>& data);

    /**
     * 콜백 호출
     */
    void notify_command(WatchdogCommand cmd, const std::string& payload);
    void notify_connection_change(bool connected);

private:
    WatchdogClientConfig config_;

    // 연결 상태
    std::atomic<bool> connected_{false};
    std::atomic<bool> standalone_{true};    // 워치독 없이 실행 시 true
    std::atomic<bool> running_{false};

    // 스레드
    std::thread heartbeat_thread_;
    std::thread receive_thread_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    // 현재 상태
    mutable std::mutex status_mutex_;
    std::atomic<uint32_t> active_connections_{0};
    std::atomic<uint32_t> pending_orders_{0};
    std::atomic<uint8_t> component_status_{0};
    std::atomic<uint32_t> error_count_{0};
    std::atomic<uint32_t> warning_count_{0};
    std::atomic<uint64_t> sequence_{0};

    // 콜백
    std::mutex callbacks_mutex_;
    std::vector<CommandCallback> command_callbacks_;
    std::vector<ConnectionCallback> connection_callbacks_;

    // 통계
    std::atomic<uint64_t> heartbeat_count_{0};
    std::atomic<uint64_t> command_count_{0};
    std::atomic<int> reconnect_count_{0};
    std::chrono::steady_clock::time_point last_heartbeat_;
    mutable std::mutex time_mutex_;

    // 소켓 (플랫폼별)
    int socket_fd_{-1};
#ifdef _WIN32
    void* pipe_handle_{nullptr};
#endif
};

// =============================================================================
// 글로벌 접근자
// =============================================================================

/**
 * WatchdogClient 싱글톤 접근
 */
inline WatchdogClient& watchdog_client() {
    return WatchdogClient::instance();
}

// =============================================================================
// RAII 연결 가드
// =============================================================================

class WatchdogConnectionGuard {
public:
    explicit WatchdogConnectionGuard(const std::string& socket_path = WATCHDOG_SOCKET_PATH) {
        watchdog_client().connect(socket_path);
        if (watchdog_client().is_connected()) {
            watchdog_client().start_heartbeat();
        }
    }

    ~WatchdogConnectionGuard() {
        watchdog_client().stop_heartbeat();
        watchdog_client().disconnect();
    }

    WatchdogConnectionGuard(const WatchdogConnectionGuard&) = delete;
    WatchdogConnectionGuard& operator=(const WatchdogConnectionGuard&) = delete;

    bool is_connected() const {
        return watchdog_client().is_connected();
    }
};

}  // namespace arbitrage
