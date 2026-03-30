#include "arbitrage/infra/tcp_server.hpp"
#include "arbitrage/infra/event_bus.hpp"

#include <algorithm>
#include <cstring>

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace arbitrage {

// =============================================================================
// Constructors / Destructor
// =============================================================================

TcpServer::TcpServer()
    : started_at_(std::chrono::steady_clock::now())
{
}

TcpServer::TcpServer(const TcpServerConfig& config)
    : config_(config)
    , started_at_(std::chrono::steady_clock::now())
{
}

TcpServer::~TcpServer() {
    stop();
}

// =============================================================================
// Server control
// =============================================================================

Result<void> TcpServer::start() {
#ifdef __linux__
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return Err<void>(ErrorCode::InvalidState, "Server already running");
    }

    if (!setup_socket()) {
        running_.store(false, std::memory_order_release);
        return Err<void>(ErrorCode::NetworkError, "Failed to setup socket");
    }

    started_at_ = std::chrono::steady_clock::now();

    // Start threads
    accept_thread_ = std::thread(&TcpServer::accept_loop, this);
    io_thread_ = std::thread(&TcpServer::io_loop, this);
    ping_thread_ = std::thread(&TcpServer::ping_loop, this);

    return Ok();
#else
    return Err<void>(ErrorCode::SystemError, "TCP server only supported on Linux");
#endif
}

void TcpServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

#ifdef __linux__
    // Close server socket
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    // Disconnect all clients
    client_manager_.remove_all_clients();

    // Wait for threads
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    if (ping_thread_.joinable()) {
        ping_thread_.join();
    }
#endif
}

// =============================================================================
// Messaging (delegates to client_manager_)
// =============================================================================

bool TcpServer::send_message(int client_id, const Message& message) {
    return client_manager_.send_message(client_id, message);
}

void TcpServer::broadcast(const Message& message) {
    client_manager_.broadcast(message);
}

void TcpServer::broadcast_if(const Message& message,
                             std::function<bool(const ClientInfo&)> predicate)
{
    client_manager_.broadcast_if(message, std::move(predicate));
}

// =============================================================================
// Client management (delegates to client_manager_)
// =============================================================================

void TcpServer::disconnect_client(int client_id, const std::string& reason) {
    client_manager_.disconnect_client(client_id, reason, epoll_fd_);
}

size_t TcpServer::client_count() const {
    return client_manager_.client_count();
}

size_t TcpServer::authenticated_client_count() const {
    return client_manager_.authenticated_client_count();
}

std::vector<ClientInfo> TcpServer::get_clients() const {
    return client_manager_.get_clients();
}

std::optional<ClientInfo> TcpServer::get_client(int client_id) const {
    return client_manager_.get_client(client_id);
}

// =============================================================================
// Callbacks (delegates to client_manager_)
// =============================================================================

void TcpServer::on_message(MessageCallback callback) {
    client_manager_.on_message(std::move(callback));
}

void TcpServer::on_client_connected(ClientCallback callback) {
    client_manager_.on_client_connected(std::move(callback));
}

void TcpServer::on_client_disconnected(ClientCallback callback) {
    client_manager_.on_client_disconnected(std::move(callback));
}

void TcpServer::set_auth_callback(AuthCallback callback) {
    client_manager_.set_auth_callback(std::move(callback));
}

void TcpServer::set_event_bus(std::shared_ptr<EventBus> bus) {
    event_bus_ = bus;
}

// =============================================================================
// Statistics
// =============================================================================

TcpServer::Stats TcpServer::get_stats() const {
    auto cm_stats = client_manager_.get_stats();

    Stats stats;
    stats.total_connections = cm_stats.total_connections;
    stats.total_disconnections = cm_stats.total_disconnections;
    stats.total_messages_sent = cm_stats.total_messages_sent;
    stats.total_messages_received = cm_stats.total_messages_received;
    stats.total_bytes_sent = cm_stats.total_bytes_sent;
    stats.total_bytes_received = cm_stats.total_bytes_received;
    stats.auth_failures = cm_stats.auth_failures;
    stats.started_at = started_at_;
    return stats;
}

// =============================================================================
// Internal implementation
// =============================================================================

#ifdef __linux__

bool TcpServer::setup_socket() {
    // Create socket
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        return false;
    }

    // SO_REUSEADDR
    int opt = 1;
    if (::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Bind
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port));

    if (config_.bind_address == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) <= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
            return false;
        }
    }

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Listen
    if (::listen(server_fd_, config_.backlog) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Non-blocking
    if (!set_nonblocking(server_fd_)) {
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Create epoll
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    return true;
}

bool TcpServer::set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

void TcpServer::accept_loop() {
    while (running_.load(std::memory_order_acquire)) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = ::accept(server_fd_,
                                  reinterpret_cast<struct sockaddr*>(&client_addr),
                                  &addr_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            break;  // Error
        }

        // Max clients check
        if (client_manager_.client_count() >= static_cast<size_t>(config_.max_clients)) {
            ::close(client_fd);
            continue;
        }

        // Non-blocking
        if (!set_nonblocking(client_fd)) {
            ::close(client_fd);
            continue;
        }

        // TCP_NODELAY (disable Nagle)
        int opt = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        // Register client
        char addr_str[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
        int client_id = client_manager_.add_client(client_fd, addr_str,
                                                    ntohs(client_addr.sin_port));

        // Add to epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;  // Edge-triggered
        ev.data.fd = client_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            client_manager_.remove_client(client_id, epoll_fd_);
            continue;
        }

        // Fire connected callback
        client_manager_.fire_connected_callback(client_id);
    }
}

void TcpServer::io_loop() {
    const int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    while (running_.load(std::memory_order_acquire)) {
        int nfds = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            int client_id = client_manager_.fd_to_client_id(fd);

            if (client_id < 0) {
                continue;
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                client_manager_.remove_client(client_id, epoll_fd_);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                handle_client_read(client_id);
            }
        }
    }
}

void TcpServer::ping_loop() {
    while (running_.load(std::memory_order_acquire)) {
        // Split ping_interval into 1s chunks for fast shutdown
        auto remaining = config_.ping_interval;
        while (remaining.count() > 0 && running_.load(std::memory_order_acquire)) {
            auto sleep_chunk = std::min(remaining, std::chrono::seconds(1));
            std::this_thread::sleep_for(sleep_chunk);
            remaining -= sleep_chunk;
        }

        if (!running_.load(std::memory_order_acquire)) {
            break;
        }

        // Get timed-out clients and disconnect them
        auto timeout_clients = client_manager_.get_timed_out_clients(
            config_.auth_timeout, config_.idle_timeout);

        for (int id : timeout_clients) {
            disconnect_client(id, "Timeout");
        }

        // Send pings to idle clients
        client_manager_.send_pings(config_.ping_interval / 2);
    }
}

void TcpServer::handle_client_read(int client_id) {
    // Get the fd for this client
    auto info = client_manager_.get_client(client_id);
    if (!info || info->fd < 0) {
        return;
    }
    int fd = info->fd;

    // Receive buffer
    std::vector<uint8_t> buffer(config_.recv_buffer_size);

    while (true) {
        ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // No more data
            }
            if (errno == EINTR) {
                continue;
            }
            client_manager_.remove_client(client_id, epoll_fd_);
            return;
        }

        if (n == 0) {
            // Connection closed
            client_manager_.remove_client(client_id, epoll_fd_);
            return;
        }

        // Hand off to client manager for buffering and message parsing
        if (!client_manager_.handle_received_data(
                client_id, buffer.data(), static_cast<size_t>(n),
                epoll_fd_, config_.require_auth, config_.max_message_size))
        {
            return;  // Client was removed during processing
        }
    }
}

#else
// Non-Linux stubs

bool TcpServer::setup_socket() { return false; }
bool TcpServer::set_nonblocking(int) { return false; }
void TcpServer::accept_loop() {}
void TcpServer::io_loop() {}
void TcpServer::ping_loop() {}
void TcpServer::handle_client_read(int) {}

#endif  // __linux__

}  // namespace arbitrage
