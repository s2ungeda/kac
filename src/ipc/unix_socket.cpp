/**
 * TASK_42: Unix Domain Socket IPC 구현
 */

#include "arbitrage/ipc/unix_socket.hpp"

#include <cstring>
#include <cerrno>
#include <algorithm>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace arbitrage {

// =============================================================================
// UnixSocketServer
// =============================================================================

UnixSocketServer::UnixSocketServer(const std::string& socket_path)
    : socket_path_(socket_path)
    , logger_(Logger::get("uds.server"))
{
}

UnixSocketServer::~UnixSocketServer() {
    stop();
}

void UnixSocketServer::start() {
#ifdef __linux__
    if (running_.exchange(true, std::memory_order_acq_rel)) return;

    // 이전 소켓 파일 정리
    ::unlink(socket_path_.c_str());

    // 소켓 생성
    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        logger_->error("socket() failed: {}", std::strerror(errno));
        running_.store(false);
        return;
    }

    // 바인드
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        logger_->error("bind('{}') failed: {}", socket_path_, std::strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        running_.store(false);
        return;
    }

    if (::listen(server_fd_, 16) < 0) {
        logger_->error("listen() failed: {}", std::strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        running_.store(false);
        return;
    }

    set_nonblocking(server_fd_);

    // epoll 생성
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        logger_->error("epoll_create1() failed: {}", std::strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        running_.store(false);
        return;
    }

    // server_fd를 epoll에 등록
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);

    event_thread_ = std::thread(&UnixSocketServer::event_loop, this);
    logger_->info("UDS server started: {}", socket_path_);
#endif
}

void UnixSocketServer::stop() {
#ifdef __linux__
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    if (event_thread_.joinable()) {
        event_thread_.join();
    }

    // 클라이언트 정리
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (int fd : client_fds_) {
            ::close(fd);
        }
        client_fds_.clear();
        recv_buffers_.clear();
    }

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }

    ::unlink(socket_path_.c_str());
    logger_->info("UDS server stopped: {}", socket_path_);
#endif
}

void UnixSocketServer::on_message(MessageCallback cb) {
    on_message_cb_ = std::move(cb);
}

void UnixSocketServer::on_client_connected(ConnectCallback cb) {
    on_connect_cb_ = std::move(cb);
}

void UnixSocketServer::on_client_disconnected(ConnectCallback cb) {
    on_disconnect_cb_ = std::move(cb);
}

size_t UnixSocketServer::client_count() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(clients_mutex_));
    return client_fds_.size();
}

void UnixSocketServer::broadcast(uint8_t type, const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (int fd : client_fds_) {
        send_frame(fd, type, data, len);
    }
}

void UnixSocketServer::send_to(int client_fd, uint8_t type,
                                const void* data, size_t len) {
    send_frame(client_fd, type, data, len);
}

// --- Private ---

void UnixSocketServer::event_loop() {
#ifdef __linux__
    constexpr int MAX_EVENTS = 32;
    struct epoll_event events[MAX_EVENTS];

    while (running_.load(std::memory_order_acquire)) {
        int nfds = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            logger_->error("epoll_wait() failed: {}", std::strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == server_fd_) {
                handle_accept();
            } else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                remove_client(fd);
            } else if (events[i].events & EPOLLIN) {
                handle_client_read(fd);
            }
        }
    }
#endif
}

void UnixSocketServer::handle_accept() {
#ifdef __linux__
    while (true) {
        int client_fd = ::accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            logger_->warn("accept() failed: {}", std::strerror(errno));
            break;
        }

        set_nonblocking(client_fd);

        struct epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            ::close(client_fd);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            client_fds_.push_back(client_fd);
            recv_buffers_[client_fd] = {};
        }

        logger_->debug("Client connected: fd={}", client_fd);
        if (on_connect_cb_) on_connect_cb_(client_fd);
    }
#endif
}

void UnixSocketServer::handle_client_read(int fd) {
#ifdef __linux__
    uint8_t tmp[4096];

    while (true) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            remove_client(fd);
            return;
        }
        if (n == 0) {
            remove_client(fd);
            return;
        }

        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto& buf = recv_buffers_[fd];
        buf.insert(buf.end(), tmp, tmp + n);

        // 프레임 파싱
        while (buf.size() >= IPC_HEADER_SIZE) {
            auto hdr = decode_ipc_header(buf.data());
            if (hdr.payload_length > IPC_MAX_PAYLOAD) {
                // 프로토콜 에러
                logger_->error("Oversized payload from fd={}: {}", fd, hdr.payload_length);
                remove_client(fd);
                return;
            }
            size_t frame_size = IPC_HEADER_SIZE + hdr.payload_length;
            if (buf.size() < frame_size) break;  // 불완전 프레임

            if (on_message_cb_) {
                on_message_cb_(fd, hdr.msg_type,
                               buf.data() + IPC_HEADER_SIZE,
                               hdr.payload_length);
            }

            buf.erase(buf.begin(), buf.begin() + frame_size);
        }
    }
#endif
}

void UnixSocketServer::remove_client(int fd) {
#ifdef __linux__
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        client_fds_.erase(
            std::remove(client_fds_.begin(), client_fds_.end(), fd),
            client_fds_.end());
        recv_buffers_.erase(fd);
    }

    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);

    logger_->debug("Client disconnected: fd={}", fd);
    if (on_disconnect_cb_) on_disconnect_cb_(fd);
#endif
}

bool UnixSocketServer::set_nonblocking(int fd) {
#ifdef __linux__
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
#else
    return false;
#endif
}

bool UnixSocketServer::send_frame(int fd, uint8_t type,
                                   const void* data, size_t len) {
#ifdef __linux__
    auto header = encode_ipc_header(type, static_cast<uint32_t>(len));

    // iovec로 scatter-gather write
    struct iovec iov[2];
    iov[0].iov_base = header.data();
    iov[0].iov_len = IPC_HEADER_SIZE;
    iov[1].iov_base = const_cast<void*>(data);
    iov[1].iov_len = len;

    struct msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = (len > 0) ? 2 : 1;

    ssize_t sent = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(IPC_HEADER_SIZE + len);
#else
    return false;
#endif
}

// =============================================================================
// UnixSocketClient
// =============================================================================

UnixSocketClient::UnixSocketClient()
    : logger_(Logger::get("uds.client"))
{
}

UnixSocketClient::~UnixSocketClient() {
    stop_recv();
    disconnect();
}

bool UnixSocketClient::connect(const std::string& socket_path) {
#ifdef __linux__
    if (connected_.load()) disconnect();

    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        logger_->error("socket() failed: {}", std::strerror(errno));
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        logger_->error("connect('{}') failed: {}", socket_path, std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    connected_.store(true, std::memory_order_release);
    logger_->info("Connected to UDS: {}", socket_path);
    return true;
#else
    return false;
#endif
}

void UnixSocketClient::disconnect() {
#ifdef __linux__
    stop_recv();

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    connected_.store(false, std::memory_order_release);
#endif
}

bool UnixSocketClient::send(uint8_t type, const void* data, size_t len) {
    return send_frame(type, data, len);
}

void UnixSocketClient::on_message(MessageCallback cb) {
    on_message_cb_ = std::move(cb);
}

void UnixSocketClient::start_recv() {
    if (recv_running_.exchange(true, std::memory_order_acq_rel)) return;
    recv_thread_ = std::thread(&UnixSocketClient::recv_loop, this);
}

void UnixSocketClient::stop_recv() {
    if (!recv_running_.exchange(false, std::memory_order_acq_rel)) return;

    // recv_loop은 recv()에서 블로킹될 수 있으므로 shutdown으로 깨움
#ifdef __linux__
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RD);
    }
#endif

    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
}

// --- Private ---

void UnixSocketClient::recv_loop() {
#ifdef __linux__
    uint8_t tmp[4096];

    while (recv_running_.load(std::memory_order_acquire) &&
           connected_.load(std::memory_order_acquire)) {
        ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                connected_.store(false, std::memory_order_release);
                logger_->info("Connection closed");
                break;
            }
            continue;
        }

        recv_buffer_.insert(recv_buffer_.end(), tmp, tmp + n);

        // 프레임 파싱
        while (recv_buffer_.size() >= IPC_HEADER_SIZE) {
            auto hdr = decode_ipc_header(recv_buffer_.data());
            if (hdr.payload_length > IPC_MAX_PAYLOAD) {
                logger_->error("Oversized payload: {}", hdr.payload_length);
                connected_.store(false, std::memory_order_release);
                return;
            }
            size_t frame_size = IPC_HEADER_SIZE + hdr.payload_length;
            if (recv_buffer_.size() < frame_size) break;

            if (on_message_cb_) {
                on_message_cb_(hdr.msg_type,
                               recv_buffer_.data() + IPC_HEADER_SIZE,
                               hdr.payload_length);
            }

            recv_buffer_.erase(recv_buffer_.begin(),
                               recv_buffer_.begin() + frame_size);
        }
    }

    recv_running_.store(false, std::memory_order_release);
#endif
}

bool UnixSocketClient::send_frame(uint8_t type, const void* data, size_t len) {
#ifdef __linux__
    if (!connected_.load(std::memory_order_acquire) || fd_ < 0) return false;

    auto header = encode_ipc_header(type, static_cast<uint32_t>(len));

    struct iovec iov[2];
    iov[0].iov_base = header.data();
    iov[0].iov_len = IPC_HEADER_SIZE;
    iov[1].iov_base = const_cast<void*>(data);
    iov[1].iov_len = len;

    struct msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = (len > 0) ? 2 : 1;

    ssize_t sent = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        logger_->error("sendmsg() failed: {}", std::strerror(errno));
        return false;
    }
    return sent == static_cast<ssize_t>(IPC_HEADER_SIZE + len);
#else
    return false;
#endif
}

}  // namespace arbitrage
