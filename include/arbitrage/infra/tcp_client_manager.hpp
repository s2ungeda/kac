#pragma once

/**
 * TCP Client Manager
 *
 * Tracks connected clients, their authentication state, receive buffers,
 * and provides broadcast / per-client send helpers.
 */

#include "arbitrage/infra/tcp_protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace arbitrage {

// =============================================================================
// Client Types
// =============================================================================

/**
 * Client connection state
 */
enum class ClientState {
    Connected,      // Connected (not yet authenticated)
    Authenticated,  // Authenticated
    Disconnecting   // Disconnecting
};

/**
 * Per-client information
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
// TCP Client Manager
// =============================================================================

class TcpClientManager {
public:
    // Callback types
    using MessageCallback = std::function<void(int client_id, const Message&)>;
    using ClientCallback = std::function<void(int client_id, const ClientInfo&)>;
    using AuthCallback = std::function<bool(const std::string& username,
                                            const std::string& password)>;

    TcpClientManager() = default;
    ~TcpClientManager() = default;

    TcpClientManager(const TcpClientManager&) = delete;
    TcpClientManager& operator=(const TcpClientManager&) = delete;

    // =========================================================================
    // Client lifecycle
    // =========================================================================

    /**
     * Register a new client. Returns assigned client_id.
     */
    int add_client(int fd, const std::string& addr, int port);

    /**
     * Remove a client (closes fd via epoll_fd, fires disconnected callback).
     * @param epoll_fd  epoll file descriptor (for EPOLL_CTL_DEL); pass -1 to skip.
     */
    void remove_client(int client_id, int epoll_fd);

    /**
     * Close and remove all clients.
     */
    void remove_all_clients();

    // =========================================================================
    // Queries
    // =========================================================================

    size_t client_count() const;
    size_t authenticated_client_count() const;
    std::vector<ClientInfo> get_clients() const;
    std::optional<ClientInfo> get_client(int client_id) const;

    /**
     * Resolve fd -> client_id. Returns -1 if not found.
     */
    int fd_to_client_id(int fd) const;

    // =========================================================================
    // Sending
    // =========================================================================

    /**
     * Send a message to one client. Returns true on full send.
     */
    bool send_message(int client_id, const Message& message);

    /**
     * Broadcast to all authenticated clients.
     */
    void broadcast(const Message& message);

    /**
     * Broadcast to clients matching a predicate.
     */
    void broadcast_if(const Message& message,
                      std::function<bool(const ClientInfo&)> predicate);

    /**
     * Disconnect a client with an optional reason string.
     * @param epoll_fd  passed through to remove_client().
     */
    void disconnect_client(int client_id, const std::string& reason, int epoll_fd);

    // =========================================================================
    // Message parsing / processing
    // =========================================================================

    /**
     * Append received bytes to a client's recv buffer, then attempt to
     * parse and dispatch complete messages.
     *
     * @param epoll_fd        forwarded to disconnect on error.
     * @param require_auth    whether unauthenticated commands should be rejected.
     * @param max_message_size  maximum allowed recv buffer size before disconnect.
     * @return false if the client was removed during processing.
     */
    bool handle_received_data(int client_id, const uint8_t* data, size_t len,
                              int epoll_fd, bool require_auth,
                              size_t max_message_size);

    /**
     * Handle authentication for a client.
     */
    void handle_auth(int client_id, const Message& message,
                     bool require_auth, int epoll_fd);

    // =========================================================================
    // Callbacks
    // =========================================================================

    void on_message(MessageCallback callback);
    void on_client_connected(ClientCallback callback);
    void on_client_disconnected(ClientCallback callback);
    void set_auth_callback(AuthCallback callback);

    // =========================================================================
    // Ping / Timeout helpers
    // =========================================================================

    /**
     * Returns list of client_ids that have exceeded their timeout.
     */
    std::vector<int> get_timed_out_clients(
        std::chrono::seconds auth_timeout,
        std::chrono::seconds idle_timeout) const;

    /**
     * Send a ping to clients whose idle time exceeds the given threshold.
     */
    void send_pings(std::chrono::seconds threshold);

    // =========================================================================
    // Statistics (atomic counters)
    // =========================================================================

    struct Stats {
        uint64_t total_connections{0};
        uint64_t total_disconnections{0};
        uint64_t total_messages_sent{0};
        uint64_t total_messages_received{0};
        uint64_t total_bytes_sent{0};
        uint64_t total_bytes_received{0};
        uint64_t auth_failures{0};
    };

    Stats get_stats() const;

    // Expose the connected callback for accept_loop usage
    void fire_connected_callback(int client_id);

private:
    void process_message(int client_id, const Message& message,
                         bool require_auth, int epoll_fd);

    // Client state (guarded by clients_mutex_)
    mutable std::mutex clients_mutex_;
    std::unordered_map<int, ClientInfo> clients_;
    std::unordered_map<int, int> fd_to_client_;
    std::unordered_map<int, std::vector<uint8_t>> recv_buffers_;
    std::atomic<int> next_client_id_{1};

    // Callbacks (guarded by callbacks_mutex_)
    std::mutex callbacks_mutex_;
    MessageCallback on_message_callback_;
    ClientCallback on_connected_callback_;
    ClientCallback on_disconnected_callback_;
    AuthCallback auth_callback_;

    // Statistics
    std::atomic<uint64_t> total_connections_{0};
    std::atomic<uint64_t> total_disconnections_{0};
    std::atomic<uint64_t> total_messages_sent_{0};
    std::atomic<uint64_t> total_messages_received_{0};
    std::atomic<uint64_t> total_bytes_sent_{0};
    std::atomic<uint64_t> total_bytes_received_{0};
    std::atomic<uint64_t> auth_failures_{0};
};

}  // namespace arbitrage
