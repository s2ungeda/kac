#include "arbitrage/infra/tcp_client_manager.hpp"

#include <algorithm>
#include <cstring>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace arbitrage {

// =============================================================================
// Client lifecycle
// =============================================================================

int TcpClientManager::add_client(int fd, const std::string& addr, int port) {
    int client_id = next_client_id_.fetch_add(1, std::memory_order_relaxed);

    ClientInfo info;
    info.id = client_id;
    info.fd = fd;
    info.state = ClientState::Connected;
    info.address = addr;
    info.port = port;
    info.connected_at = std::chrono::steady_clock::now();
    info.last_activity = info.connected_at;

    {
        WriteGuard lock(clients_mutex_);
        clients_[client_id] = info;
        fd_to_client_[fd] = client_id;
        recv_buffers_[client_id] = {};
    }

    total_connections_.fetch_add(1, std::memory_order_relaxed);
    return client_id;
}

void TcpClientManager::remove_client(int client_id, int epoll_fd) {
    ClientInfo info;
    bool found = false;

    {
        WriteGuard lock(clients_mutex_);

        auto it = clients_.find(client_id);
        if (it == clients_.end()) {
            return;
        }

        info = it->second;
        found = true;

#ifdef __linux__
        // Remove from epoll and close socket
        if (info.fd >= 0) {
            if (epoll_fd >= 0) {
                ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, info.fd, nullptr);
            }
            ::close(info.fd);
        }
#else
        (void)epoll_fd;
#endif

        fd_to_client_.erase(info.fd);
        recv_buffers_.erase(client_id);
        clients_.erase(it);
    }

    if (found) {
        total_disconnections_.fetch_add(1, std::memory_order_relaxed);

        // Fire disconnected callback
        ClientCallback cb;
        {
            SpinLockGuard lock(callbacks_mutex_);
            cb = on_disconnected_callback_;
        }

        if (cb) {
            try {
                cb(client_id, info);
            } catch (const std::exception& e) {
                // Callback error logged; processing continues
            }
        }
    }
}

void TcpClientManager::remove_all_clients() {
    WriteGuard lock(clients_mutex_);
    for (auto& [id, info] : clients_) {
#ifdef __linux__
        if (info.fd >= 0) {
            ::close(info.fd);
        }
#endif
    }
    clients_.clear();
    fd_to_client_.clear();
    recv_buffers_.clear();
}

// =============================================================================
// Queries
// =============================================================================

size_t TcpClientManager::client_count() const {
    ReadGuard lock(clients_mutex_);
    return clients_.size();
}

size_t TcpClientManager::authenticated_client_count() const {
    ReadGuard lock(clients_mutex_);
    size_t count = 0;
    for (const auto& [id, info] : clients_) {
        if (info.state == ClientState::Authenticated) {
            ++count;
        }
    }
    return count;
}

std::vector<ClientInfo> TcpClientManager::get_clients() const {
    ReadGuard lock(clients_mutex_);
    std::vector<ClientInfo> result;
    result.reserve(clients_.size());
    for (const auto& [id, info] : clients_) {
        result.push_back(info);
    }
    return result;
}

std::optional<ClientInfo> TcpClientManager::get_client(int client_id) const {
    ReadGuard lock(clients_mutex_);
    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
        return it->second;
    }
    return std::nullopt;
}

int TcpClientManager::fd_to_client_id(int fd) const {
    ReadGuard lock(clients_mutex_);
    auto it = fd_to_client_.find(fd);
    if (it != fd_to_client_.end()) {
        return it->second;
    }
    return -1;
}

// =============================================================================
// Sending
// =============================================================================

bool TcpClientManager::send_message(int client_id, const Message& message) {
#ifdef __linux__
    WriteGuard lock(clients_mutex_);

    auto it = clients_.find(client_id);
    if (it == clients_.end() || it->second.fd < 0) {
        return false;
    }

    auto data = message.serialize();
    ssize_t sent = ::send(it->second.fd, data.data(), data.size(), MSG_NOSIGNAL);

    if (sent > 0) {
        it->second.bytes_sent += static_cast<uint64_t>(sent);
        it->second.messages_sent++;
        total_bytes_sent_.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
        total_messages_sent_.fetch_add(1, std::memory_order_relaxed);
        return sent == static_cast<ssize_t>(data.size());
    }

    return false;
#else
    (void)client_id;
    (void)message;
    return false;
#endif
}

void TcpClientManager::broadcast(const Message& message) {
    broadcast_if(message, [](const ClientInfo& info) {
        return info.state == ClientState::Authenticated;
    });
}

void TcpClientManager::broadcast_if(const Message& message,
                                     std::function<bool(const ClientInfo&)> predicate)
{
#ifdef __linux__
    auto data = message.serialize();

    WriteGuard lock(clients_mutex_);

    for (auto& [id, info] : clients_) {
        if (info.fd >= 0 && predicate(info)) {
            ssize_t sent = ::send(info.fd, data.data(), data.size(), MSG_NOSIGNAL);
            if (sent > 0) {
                info.bytes_sent += static_cast<uint64_t>(sent);
                info.messages_sent++;
                total_bytes_sent_.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
                total_messages_sent_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
#else
    (void)message;
    (void)predicate;
#endif
}

void TcpClientManager::disconnect_client(int client_id, const std::string& reason,
                                          int epoll_fd)
{
#ifdef __linux__
    {
        WriteGuard lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it != clients_.end()) {
            // Send disconnect message before removal
            if (!reason.empty() && it->second.fd >= 0) {
                Message disconnect_msg(MessageType::Disconnect, reason);
                auto data = disconnect_msg.serialize();
                ::send(it->second.fd, data.data(), data.size(), MSG_NOSIGNAL);
            }
        } else {
            return;
        }
    }

    remove_client(client_id, epoll_fd);
#else
    (void)client_id;
    (void)reason;
    (void)epoll_fd;
#endif
}

// =============================================================================
// Message parsing / processing
// =============================================================================

bool TcpClientManager::handle_received_data(int client_id, const uint8_t* data,
                                              size_t len, int epoll_fd,
                                              bool require_auth,
                                              size_t max_message_size)
{
    // Append to receive buffer and update stats
    {
        bool too_large = false;
        {
            WriteGuard lock(clients_mutex_);

            auto it = clients_.find(client_id);
            if (it == clients_.end()) {
                return false;
            }

            it->second.last_activity = std::chrono::steady_clock::now();
            it->second.bytes_received += static_cast<uint64_t>(len);
            total_bytes_received_.fetch_add(static_cast<uint64_t>(len), std::memory_order_relaxed);

            auto& recv_buf = recv_buffers_[client_id];
            recv_buf.insert(recv_buf.end(), data, data + len);

            // Max message size check
            if (recv_buf.size() > max_message_size) {
                too_large = true;
            }
        }
        if (too_large) {
            disconnect_client(client_id, "Message too large", epoll_fd);
            return false;
        }
    }

    // Parse complete messages
    while (true) {
        Message msg;
        bool need_disconnect = false;
        bool need_more_data = false;
        bool buffer_gone = false;

        {
            WriteGuard lock(clients_mutex_);
            auto buf_it = recv_buffers_.find(client_id);
            if (buf_it == recv_buffers_.end()) {
                buffer_gone = true;
            } else {
                auto* recv_buf = &buf_it->second;

                // Need at least a header
                if (recv_buf->size() < sizeof(MessageHeader)) {
                    need_more_data = true;
                } else {
                    // Parse header
                    MessageHeader header;
                    std::memcpy(&header, recv_buf->data(), sizeof(MessageHeader));

                    if (!header.is_valid()) {
                        need_disconnect = true;
                    } else {
                        // Check if full message is available
                        size_t total_size = sizeof(MessageHeader) + header.payload_length;
                        if (recv_buf->size() < total_size) {
                            need_more_data = true;
                        } else {
                            // Extract message
                            msg.header = header;
                            msg.payload.assign(
                                recv_buf->begin() + sizeof(MessageHeader),
                                recv_buf->begin() + total_size
                            );

                            // Remove from buffer
                            recv_buf->erase(recv_buf->begin(), recv_buf->begin() + total_size);

                            auto client_it = clients_.find(client_id);
                            if (client_it != clients_.end()) {
                                client_it->second.messages_received++;
                            }
                            total_messages_received_.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        }

        if (buffer_gone) {
            return false;
        }
        if (need_more_data) {
            break;
        }
        if (need_disconnect) {
            disconnect_client(client_id, "Invalid message header", epoll_fd);
            return false;
        }

        // Process message outside lock
        process_message(client_id, msg, require_auth, epoll_fd);
    }

    return true;
}

void TcpClientManager::process_message(int client_id, const Message& message,
                                        bool require_auth, int epoll_fd)
{
    MessageType type = message.header.type();

    // Pong response
    if (type == MessageType::Ping) {
        Message pong(MessageType::Pong);
        send_message(client_id, pong);
        return;
    }

    // Auth request
    if (type == MessageType::AuthRequest) {
        handle_auth(client_id, message, require_auth, epoll_fd);
        return;
    }

    // Auth check
    if (require_auth) {
        ReadGuard lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it == clients_.end() || it->second.state != ClientState::Authenticated) {
            Message error(MessageType::Error, "Authentication required");
            send_message(client_id, error);
            return;
        }
    }

    // Fire message callback
    MessageCallback cb;
    {
        SpinLockGuard lock(callbacks_mutex_);
        cb = on_message_callback_;
    }

    if (cb) {
        try {
            cb(client_id, message);
        } catch (const std::exception& e) {
            // Message callback error logged; processing continues
        }
    }
}

void TcpClientManager::handle_auth(int client_id, const Message& message,
                                    bool require_auth, int epoll_fd)
{
    // Parse username:password from payload
    std::string payload = message.payload_str();
    size_t colon_pos = payload.find(':');

    std::string username, password;
    if (colon_pos != std::string::npos) {
        username = payload.substr(0, colon_pos);
        password = payload.substr(colon_pos + 1);
    }

    bool auth_success = false;

    // Fire auth callback
    AuthCallback cb;
    {
        SpinLockGuard lock(callbacks_mutex_);
        cb = auth_callback_;
    }

    if (cb) {
        try {
            auth_success = cb(username, password);
        } catch (...) {
            auth_success = false;
        }
    } else if (!require_auth) {
        auth_success = true;
    }

    // Send result
    if (auth_success) {
        {
            WriteGuard lock(clients_mutex_);
            auto it = clients_.find(client_id);
            if (it != clients_.end()) {
                it->second.state = ClientState::Authenticated;
                it->second.username = username;
            }
        }

        Message response(MessageType::AuthResponse, "OK");
        send_message(client_id, response);
    } else {
        auth_failures_.fetch_add(1, std::memory_order_relaxed);

        Message response(MessageType::AuthResponse, "FAILED");
        send_message(client_id, response);

        // Disconnect on auth failure
        disconnect_client(client_id, "Authentication failed", epoll_fd);
    }
}

// =============================================================================
// Callbacks
// =============================================================================

void TcpClientManager::on_message(MessageCallback callback) {
    SpinLockGuard lock(callbacks_mutex_);
    on_message_callback_ = std::move(callback);
}

void TcpClientManager::on_client_connected(ClientCallback callback) {
    SpinLockGuard lock(callbacks_mutex_);
    on_connected_callback_ = std::move(callback);
}

void TcpClientManager::on_client_disconnected(ClientCallback callback) {
    SpinLockGuard lock(callbacks_mutex_);
    on_disconnected_callback_ = std::move(callback);
}

void TcpClientManager::set_auth_callback(AuthCallback callback) {
    SpinLockGuard lock(callbacks_mutex_);
    auth_callback_ = std::move(callback);
}

void TcpClientManager::fire_connected_callback(int client_id) {
    ClientCallback cb;
    ClientInfo info;
    {
        SpinLockGuard lock(callbacks_mutex_);
        cb = on_connected_callback_;
    }
    {
        ReadGuard lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it != clients_.end()) {
            info = it->second;
        }
    }
    if (cb) {
        try {
            cb(client_id, info);
        } catch (const std::exception& e) {
            // Callback error logged; connection processing continues
        }
    }
}

// =============================================================================
// Ping / Timeout helpers
// =============================================================================

std::vector<int> TcpClientManager::get_timed_out_clients(
    std::chrono::seconds auth_timeout,
    std::chrono::seconds idle_timeout) const
{
    auto now = std::chrono::steady_clock::now();
    std::vector<int> timeout_clients;

    ReadGuard lock(clients_mutex_);
    for (const auto& [id, info] : clients_) {
        auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
            now - info.last_activity);

        // Auth timeout check
        if (info.state == ClientState::Connected) {
            auto since_connect = std::chrono::duration_cast<std::chrono::seconds>(
                now - info.connected_at);
            if (since_connect > auth_timeout) {
                timeout_clients.push_back(id);
                continue;
            }
        }

        // Idle timeout check
        if (idle_time > idle_timeout) {
            timeout_clients.push_back(id);
        }
    }

    return timeout_clients;
}

void TcpClientManager::send_pings(std::chrono::seconds threshold) {
#ifdef __linux__
    auto now = std::chrono::steady_clock::now();

    ReadGuard lock(clients_mutex_);
    for (const auto& [id, info] : clients_) {
        auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
            now - info.last_activity);

        if (idle_time > threshold && info.fd >= 0) {
            Message ping(MessageType::Ping);
            auto data = ping.serialize();
            ::send(info.fd, data.data(), data.size(), MSG_NOSIGNAL);
        }
    }
#else
    (void)threshold;
#endif
}

// =============================================================================
// Statistics
// =============================================================================

TcpClientManager::Stats TcpClientManager::get_stats() const {
    Stats stats;
    stats.total_connections = total_connections_.load(std::memory_order_relaxed);
    stats.total_disconnections = total_disconnections_.load(std::memory_order_relaxed);
    stats.total_messages_sent = total_messages_sent_.load(std::memory_order_relaxed);
    stats.total_messages_received = total_messages_received_.load(std::memory_order_relaxed);
    stats.total_bytes_sent = total_bytes_sent_.load(std::memory_order_relaxed);
    stats.total_bytes_received = total_bytes_received_.load(std::memory_order_relaxed);
    stats.auth_failures = auth_failures_.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace arbitrage
