#pragma once

/**
 * TCP Server (TASK_23)
 *
 * Delphi 클라이언트 연동을 위한 TCP 서버
 * - 다중 클라이언트 지원
 * - 간단한 바이너리 프로토콜
 * - 인증 지원
 * - 브로드캐스트
 */

#include "arbitrage/common/error.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace arbitrage {

// Forward declarations
class EventBus;

// =============================================================================
// 프로토콜 정의
// =============================================================================

/**
 * 메시지 타입
 */
enum class MessageType : uint8_t {
    // 연결/인증
    Ping = 0x01,
    Pong = 0x02,
    AuthRequest = 0x10,
    AuthResponse = 0x11,
    Disconnect = 0x1F,

    // 시세 데이터
    TickerUpdate = 0x20,
    OrderBookUpdate = 0x21,
    PremiumUpdate = 0x22,
    OpportunityAlert = 0x23,

    // 주문/거래
    OrderStatus = 0x30,
    TradeResult = 0x31,
    BalanceUpdate = 0x32,

    // 시스템
    SystemStatus = 0x40,
    HealthStatus = 0x41,
    KillSwitch = 0x42,
    ConfigUpdate = 0x43,

    // 명령 (클라이언트 -> 서버)
    CmdStartStrategy = 0x80,
    CmdStopStrategy = 0x81,
    CmdSetKillSwitch = 0x82,
    CmdGetStatus = 0x83,

    // 응답
    CmdResponse = 0xF0,
    Error = 0xFF
};

/**
 * 메시지 타입 이름
 */
inline const char* message_type_name(MessageType type) {
    switch (type) {
        case MessageType::Ping: return "Ping";
        case MessageType::Pong: return "Pong";
        case MessageType::AuthRequest: return "AuthRequest";
        case MessageType::AuthResponse: return "AuthResponse";
        case MessageType::Disconnect: return "Disconnect";
        case MessageType::TickerUpdate: return "TickerUpdate";
        case MessageType::OrderBookUpdate: return "OrderBookUpdate";
        case MessageType::PremiumUpdate: return "PremiumUpdate";
        case MessageType::OpportunityAlert: return "OpportunityAlert";
        case MessageType::OrderStatus: return "OrderStatus";
        case MessageType::TradeResult: return "TradeResult";
        case MessageType::BalanceUpdate: return "BalanceUpdate";
        case MessageType::SystemStatus: return "SystemStatus";
        case MessageType::HealthStatus: return "HealthStatus";
        case MessageType::KillSwitch: return "KillSwitch";
        case MessageType::ConfigUpdate: return "ConfigUpdate";
        case MessageType::CmdStartStrategy: return "CmdStartStrategy";
        case MessageType::CmdStopStrategy: return "CmdStopStrategy";
        case MessageType::CmdSetKillSwitch: return "CmdSetKillSwitch";
        case MessageType::CmdGetStatus: return "CmdGetStatus";
        case MessageType::CmdResponse: return "CmdResponse";
        case MessageType::Error: return "Error";
        default: return "Unknown";
    }
}

/**
 * 메시지 헤더
 *
 * | Magic (2) | Version (1) | Type (1) | Payload Length (4) |
 * | 0x4B 0x41 |     0x01    |   type   |     length         |
 */
#pragma pack(push, 1)
struct MessageHeader {
    uint8_t magic[2]{0x4B, 0x41};  // "KA" (Kimchi Arbitrage)
    uint8_t version{0x01};
    uint8_t msg_type{0};
    uint32_t payload_length{0};

    bool is_valid() const {
        return magic[0] == 0x4B && magic[1] == 0x41 && version == 0x01;
    }

    MessageType type() const {
        return static_cast<MessageType>(msg_type);
    }

    void set_type(MessageType t) {
        msg_type = static_cast<uint8_t>(t);
    }
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 8, "MessageHeader must be 8 bytes");

/**
 * 메시지 (헤더 + 페이로드)
 */
struct Message {
    MessageHeader header;
    std::vector<uint8_t> payload;

    Message() = default;
    Message(MessageType type, const std::vector<uint8_t>& data = {})
        : payload(data)
    {
        header.set_type(type);
        header.payload_length = static_cast<uint32_t>(data.size());
    }

    Message(MessageType type, const std::string& data)
        : payload(data.begin(), data.end())
    {
        header.set_type(type);
        header.payload_length = static_cast<uint32_t>(data.size());
    }

    std::string payload_str() const {
        return std::string(payload.begin(), payload.end());
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> result;
        result.reserve(sizeof(MessageHeader) + payload.size());

        // Header
        const uint8_t* hdr = reinterpret_cast<const uint8_t*>(&header);
        result.insert(result.end(), hdr, hdr + sizeof(MessageHeader));

        // Payload
        result.insert(result.end(), payload.begin(), payload.end());

        return result;
    }
};

// =============================================================================
// 클라이언트 정보
// =============================================================================

/**
 * 클라이언트 상태
 */
enum class ClientState {
    Connected,      // 연결됨 (미인증)
    Authenticated,  // 인증됨
    Disconnecting   // 연결 해제 중
};

/**
 * 클라이언트 정보
 */
struct ClientInfo {
    int id{-1};
    int fd{-1};
    ClientState state{ClientState::Connected};
    std::string address;
    int port{0};
    std::string username;
    std::chrono::steady_clock::time_point connected_at;
    std::chrono::steady_clock::time_point last_activity;
    uint64_t bytes_sent{0};
    uint64_t bytes_received{0};
    uint64_t messages_sent{0};
    uint64_t messages_received{0};
};

// =============================================================================
// TCP 서버 설정
// =============================================================================

struct TcpServerConfig {
    std::string bind_address{"0.0.0.0"};
    int port{9090};
    int max_clients{100};
    int backlog{128};
    std::chrono::seconds auth_timeout{30};
    std::chrono::seconds idle_timeout{300};
    std::chrono::seconds ping_interval{30};
    bool require_auth{true};
    size_t max_message_size{1024 * 1024};  // 1MB
    size_t recv_buffer_size{65536};        // 64KB
};

// =============================================================================
// TCP 서버
// =============================================================================

class TcpServer {
public:
    // 콜백 타입
    using MessageCallback = std::function<void(int client_id, const Message&)>;
    using ClientCallback = std::function<void(int client_id, const ClientInfo&)>;
    using AuthCallback = std::function<bool(const std::string& username,
                                            const std::string& password)>;

    /**
     * 생성자
     */
    TcpServer();
    explicit TcpServer(const TcpServerConfig& config);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // =========================================================================
    // 서버 제어
    // =========================================================================

    /**
     * 서버 시작
     */
    Result<void> start();

    /**
     * 서버 중지
     */
    void stop();

    /**
     * 실행 중인지 확인
     */
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // =========================================================================
    // 메시지 전송
    // =========================================================================

    /**
     * 특정 클라이언트에 메시지 전송
     */
    bool send_message(int client_id, const Message& message);

    /**
     * 인증된 모든 클라이언트에 브로드캐스트
     */
    void broadcast(const Message& message);

    /**
     * 조건부 브로드캐스트
     */
    void broadcast_if(const Message& message,
                      std::function<bool(const ClientInfo&)> predicate);

    // =========================================================================
    // 클라이언트 관리
    // =========================================================================

    /**
     * 클라이언트 연결 해제
     */
    void disconnect_client(int client_id, const std::string& reason = "");

    /**
     * 연결된 클라이언트 수
     */
    size_t client_count() const;

    /**
     * 인증된 클라이언트 수
     */
    size_t authenticated_client_count() const;

    /**
     * 클라이언트 정보 조회
     */
    std::vector<ClientInfo> get_clients() const;

    /**
     * 특정 클라이언트 정보
     */
    std::optional<ClientInfo> get_client(int client_id) const;

    // =========================================================================
    // 콜백 설정
    // =========================================================================

    /**
     * 메시지 수신 콜백
     */
    void on_message(MessageCallback callback);

    /**
     * 클라이언트 연결 콜백
     */
    void on_client_connected(ClientCallback callback);

    /**
     * 클라이언트 연결 해제 콜백
     */
    void on_client_disconnected(ClientCallback callback);

    /**
     * 인증 콜백 설정
     */
    void set_auth_callback(AuthCallback callback);

    // =========================================================================
    // EventBus 연동
    // =========================================================================

    /**
     * EventBus 연결
     */
    void set_event_bus(std::shared_ptr<EventBus> bus);

    // =========================================================================
    // 설정
    // =========================================================================

    /**
     * 설정 조회
     */
    const TcpServerConfig& config() const { return config_; }

    // =========================================================================
    // 통계
    // =========================================================================

    struct Stats {
        uint64_t total_connections{0};
        uint64_t total_disconnections{0};
        uint64_t total_messages_sent{0};
        uint64_t total_messages_received{0};
        uint64_t total_bytes_sent{0};
        uint64_t total_bytes_received{0};
        uint64_t auth_failures{0};
        std::chrono::steady_clock::time_point started_at;
    };

    Stats get_stats() const;

private:
    // =========================================================================
    // 내부 구현
    // =========================================================================

    /**
     * Accept 루프
     */
    void accept_loop();

    /**
     * I/O 루프 (epoll 기반)
     */
    void io_loop();

    /**
     * 핑 체크 루프
     */
    void ping_loop();

    /**
     * 클라이언트 데이터 읽기
     */
    void handle_client_read(int client_id);

    /**
     * 메시지 처리
     */
    void process_message(int client_id, const Message& message);

    /**
     * 인증 처리
     */
    void handle_auth(int client_id, const Message& message);

    /**
     * 소켓 설정
     */
    bool setup_socket();

    /**
     * 소켓 non-blocking 설정
     */
    bool set_nonblocking(int fd);

    /**
     * 클라이언트 추가/제거
     */
    int add_client(int fd, const std::string& addr, int port);
    void remove_client(int client_id);

private:
    TcpServerConfig config_;

    // 소켓
    int server_fd_{-1};
    int epoll_fd_{-1};

    // 스레드
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::thread io_thread_;
    std::thread ping_thread_;

    // 클라이언트 관리
    mutable std::mutex clients_mutex_;
    std::unordered_map<int, ClientInfo> clients_;           // client_id -> info
    std::unordered_map<int, int> fd_to_client_;             // fd -> client_id
    std::unordered_map<int, std::vector<uint8_t>> recv_buffers_;  // client_id -> buffer
    std::atomic<int> next_client_id_{1};

    // 콜백
    std::mutex callbacks_mutex_;
    MessageCallback on_message_callback_;
    ClientCallback on_connected_callback_;
    ClientCallback on_disconnected_callback_;
    AuthCallback auth_callback_;

    // EventBus
    std::weak_ptr<EventBus> event_bus_;

    // 통계
    std::atomic<uint64_t> total_connections_{0};
    std::atomic<uint64_t> total_disconnections_{0};
    std::atomic<uint64_t> total_messages_sent_{0};
    std::atomic<uint64_t> total_messages_received_{0};
    std::atomic<uint64_t> total_bytes_sent_{0};
    std::atomic<uint64_t> total_bytes_received_{0};
    std::atomic<uint64_t> auth_failures_{0};
    std::chrono::steady_clock::time_point started_at_;
};

// =============================================================================
// 편의 함수
// =============================================================================

/**
 * 간단한 JSON 형식 페이로드 생성
 * (실제 JSON 라이브러리 대신 간단한 문자열 구성)
 */
inline std::string make_json_payload(
    const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::string json = "{";
    bool first = true;
    for (const auto& [key, value] : fields) {
        if (!first) json += ",";
        json += "\"" + key + "\":\"" + value + "\"";
        first = false;
    }
    json += "}";
    return json;
}

/**
 * 숫자 포함 JSON 페이로드 생성
 */
inline std::string make_json_payload_num(
    const std::vector<std::pair<std::string, std::string>>& str_fields,
    const std::vector<std::pair<std::string, double>>& num_fields)
{
    std::string json = "{";
    bool first = true;

    for (const auto& [key, value] : str_fields) {
        if (!first) json += ",";
        json += "\"" + key + "\":\"" + value + "\"";
        first = false;
    }

    for (const auto& [key, value] : num_fields) {
        if (!first) json += ",";
        json += "\"" + key + "\":" + std::to_string(value);
        first = false;
    }

    json += "}";
    return json;
}

}  // namespace arbitrage
