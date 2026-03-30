#pragma once

/**
 * TCP Server (TASK_23)
 *
 * Delphi client TCP server orchestrator.
 * - epoll-based I/O loop
 * - Delegates protocol handling to TcpProtocol (tcp_protocol.hpp)
 * - Delegates client tracking to TcpClientManager (tcp_client_manager.hpp)
 *
 * Public API is unchanged from the original monolithic version.
 */

// Re-export protocol and client-manager types so that callers who
// only #include "tcp_server.hpp" still see MessageType, Message,
// ClientInfo, ClientState, make_json_payload, etc.
#include "arbitrage/infra/tcp_protocol.hpp"
#include "arbitrage/infra/tcp_client_manager.hpp"

#include "arbitrage/common/error.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace arbitrage {

// Forward declarations
class EventBus;

// =============================================================================
// TCP Server Config
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
// TCP Server
// =============================================================================

class TcpServer {
public:
    // Callback types (forwarded from TcpClientManager for API compatibility)
    using MessageCallback = TcpClientManager::MessageCallback;
    using ClientCallback = TcpClientManager::ClientCallback;
    using AuthCallback = TcpClientManager::AuthCallback;

    /**
     * Constructors
     */
    TcpServer();
    explicit TcpServer(const TcpServerConfig& config);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // =========================================================================
    // Server control
    // =========================================================================

    Result<void> start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // =========================================================================
    // Messaging (delegates to client_manager_)
    // =========================================================================

    bool send_message(int client_id, const Message& message);
    void broadcast(const Message& message);
    void broadcast_if(const Message& message,
                      std::function<bool(const ClientInfo&)> predicate);

    // =========================================================================
    // Client management (delegates to client_manager_)
    // =========================================================================

    void disconnect_client(int client_id, const std::string& reason = "");
    size_t client_count() const;
    size_t authenticated_client_count() const;
    std::vector<ClientInfo> get_clients() const;
    std::optional<ClientInfo> get_client(int client_id) const;

    // =========================================================================
    // Callbacks (delegates to client_manager_)
    // =========================================================================

    void on_message(MessageCallback callback);
    void on_client_connected(ClientCallback callback);
    void on_client_disconnected(ClientCallback callback);
    void set_auth_callback(AuthCallback callback);

    // =========================================================================
    // EventBus
    // =========================================================================

    void set_event_bus(std::shared_ptr<EventBus> bus);

    // =========================================================================
    // Config
    // =========================================================================

    const TcpServerConfig& config() const { return config_; }

    // =========================================================================
    // Statistics
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
    // Internal implementation
    // =========================================================================

    void accept_loop();
    void io_loop();
    void ping_loop();
    void handle_client_read(int client_id);
    bool setup_socket();
    bool set_nonblocking(int fd);

private:
    TcpServerConfig config_;

    // Sockets
    int server_fd_{-1};
    int epoll_fd_{-1};

    // Threads
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::thread io_thread_;
    std::thread ping_thread_;

    // Client manager (owns all client state, callbacks, send/recv)
    TcpClientManager client_manager_;

    // EventBus
    std::weak_ptr<EventBus> event_bus_;

    // Server start time
    std::chrono::steady_clock::time_point started_at_;
};

}  // namespace arbitrage
