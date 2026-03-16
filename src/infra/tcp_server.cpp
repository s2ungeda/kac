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
// 생성자/소멸자
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
// 서버 제어
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

    // 스레드 시작
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
    // 소켓 닫기
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    // 모든 클라이언트 연결 해제
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& [id, info] : clients_) {
            if (info.fd >= 0) {
                ::close(info.fd);
            }
        }
        clients_.clear();
        fd_to_client_.clear();
        recv_buffers_.clear();
    }

    // 스레드 종료 대기
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
// 메시지 전송
// =============================================================================

bool TcpServer::send_message(int client_id, const Message& message) {
#ifdef __linux__
    std::lock_guard<std::mutex> lock(clients_mutex_);

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

void TcpServer::broadcast(const Message& message) {
    broadcast_if(message, [](const ClientInfo& info) {
        return info.state == ClientState::Authenticated;
    });
}

void TcpServer::broadcast_if(const Message& message,
                             std::function<bool(const ClientInfo&)> predicate)
{
#ifdef __linux__
    auto data = message.serialize();

    std::lock_guard<std::mutex> lock(clients_mutex_);

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

// =============================================================================
// 클라이언트 관리
// =============================================================================

void TcpServer::disconnect_client(int client_id, const std::string& reason) {
#ifdef __linux__
    ClientInfo info;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it != clients_.end()) {
            info = it->second;
            found = true;

            // 종료 메시지 전송
            if (!reason.empty() && info.fd >= 0) {
                Message disconnect_msg(MessageType::Disconnect, reason);
                auto data = disconnect_msg.serialize();
                ::send(info.fd, data.data(), data.size(), MSG_NOSIGNAL);
            }
        }
    }

    if (found) {
        remove_client(client_id);
    }
#else
    (void)client_id;
    (void)reason;
#endif
}

size_t TcpServer::client_count() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return clients_.size();
}

size_t TcpServer::authenticated_client_count() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    size_t count = 0;
    for (const auto& [id, info] : clients_) {
        if (info.state == ClientState::Authenticated) {
            ++count;
        }
    }
    return count;
}

std::vector<ClientInfo> TcpServer::get_clients() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    std::vector<ClientInfo> result;
    result.reserve(clients_.size());
    for (const auto& [id, info] : clients_) {
        result.push_back(info);
    }
    return result;
}

std::optional<ClientInfo> TcpServer::get_client(int client_id) const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
        return it->second;
    }
    return std::nullopt;
}

// =============================================================================
// 콜백 설정
// =============================================================================

void TcpServer::on_message(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    on_message_callback_ = std::move(callback);
}

void TcpServer::on_client_connected(ClientCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    on_connected_callback_ = std::move(callback);
}

void TcpServer::on_client_disconnected(ClientCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    on_disconnected_callback_ = std::move(callback);
}

void TcpServer::set_auth_callback(AuthCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auth_callback_ = std::move(callback);
}

void TcpServer::set_event_bus(std::shared_ptr<EventBus> bus) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    event_bus_ = bus;
}

// =============================================================================
// 통계
// =============================================================================

TcpServer::Stats TcpServer::get_stats() const {
    Stats stats;
    stats.total_connections = total_connections_.load(std::memory_order_relaxed);
    stats.total_disconnections = total_disconnections_.load(std::memory_order_relaxed);
    stats.total_messages_sent = total_messages_sent_.load(std::memory_order_relaxed);
    stats.total_messages_received = total_messages_received_.load(std::memory_order_relaxed);
    stats.total_bytes_sent = total_bytes_sent_.load(std::memory_order_relaxed);
    stats.total_bytes_received = total_bytes_received_.load(std::memory_order_relaxed);
    stats.auth_failures = auth_failures_.load(std::memory_order_relaxed);
    stats.started_at = started_at_;
    return stats;
}

// =============================================================================
// 내부 구현
// =============================================================================

#ifdef __linux__

bool TcpServer::setup_socket() {
    // 소켓 생성
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        return false;
    }

    // SO_REUSEADDR 설정
    int opt = 1;
    if (::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // 바인드
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

    // 리슨
    if (::listen(server_fd_, config_.backlog) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Non-blocking 설정
    if (!set_nonblocking(server_fd_)) {
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // epoll 생성
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
                // Non-blocking이므로 대기
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            break;  // 에러
        }

        // 최대 클라이언트 수 확인
        if (client_count() >= static_cast<size_t>(config_.max_clients)) {
            ::close(client_fd);
            continue;
        }

        // Non-blocking 설정
        if (!set_nonblocking(client_fd)) {
            ::close(client_fd);
            continue;
        }

        // TCP_NODELAY 설정 (Nagle 비활성화)
        int opt = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        // 클라이언트 추가
        char addr_str[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
        int client_id = add_client(client_fd, addr_str, ntohs(client_addr.sin_port));

        // epoll에 등록
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;  // Edge-triggered
        ev.data.fd = client_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            remove_client(client_id);
            continue;
        }

        total_connections_.fetch_add(1, std::memory_order_relaxed);

        // 연결 콜백
        ClientCallback cb;
        ClientInfo info;
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            cb = on_connected_callback_;
        }
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = clients_.find(client_id);
            if (it != clients_.end()) {
                info = it->second;
            }
        }
        if (cb) {
            try {
                cb(client_id, info);
            } catch (const std::exception& e) {
                // 콜백 에러 로깅 (연결 처리 계속)
            }
        }
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
            int client_id = -1;

            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                auto it = fd_to_client_.find(fd);
                if (it != fd_to_client_.end()) {
                    client_id = it->second;
                }
            }

            if (client_id < 0) {
                continue;
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                remove_client(client_id);
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
        std::this_thread::sleep_for(config_.ping_interval);

        if (!running_.load(std::memory_order_acquire)) {
            break;
        }

        auto now = std::chrono::steady_clock::now();
        std::vector<int> timeout_clients;

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (const auto& [id, info] : clients_) {
                auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
                    now - info.last_activity);

                // 인증 타임아웃 체크
                if (info.state == ClientState::Connected) {
                    auto since_connect = std::chrono::duration_cast<std::chrono::seconds>(
                        now - info.connected_at);
                    if (since_connect > config_.auth_timeout) {
                        timeout_clients.push_back(id);
                        continue;
                    }
                }

                // 유휴 타임아웃 체크
                if (idle_time > config_.idle_timeout) {
                    timeout_clients.push_back(id);
                    continue;
                }

                // 핑 전송
                if (idle_time > config_.ping_interval / 2) {
                    Message ping(MessageType::Ping);
                    auto data = ping.serialize();
                    if (info.fd >= 0) {
                        ::send(info.fd, data.data(), data.size(), MSG_NOSIGNAL);
                    }
                }
            }
        }

        // 타임아웃 클라이언트 연결 해제
        for (int id : timeout_clients) {
            disconnect_client(id, "Timeout");
        }
    }
}

void TcpServer::handle_client_read(int client_id) {
    int fd = -1;

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it == clients_.end()) {
            return;
        }
        fd = it->second.fd;
    }

    if (fd < 0) {
        return;
    }

    // 수신 버퍼
    std::vector<uint8_t> buffer(config_.recv_buffer_size);

    while (true) {
        ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // 더 이상 데이터 없음
            }
            if (errno == EINTR) {
                continue;
            }
            remove_client(client_id);
            return;
        }

        if (n == 0) {
            // 연결 종료
            remove_client(client_id);
            return;
        }

        // 버퍼에 추가
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);

            auto it = clients_.find(client_id);
            if (it == clients_.end()) {
                return;
            }

            it->second.last_activity = std::chrono::steady_clock::now();
            it->second.bytes_received += static_cast<uint64_t>(n);
            total_bytes_received_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

            auto& recv_buf = recv_buffers_[client_id];
            recv_buf.insert(recv_buf.end(), buffer.begin(), buffer.begin() + n);

            // 최대 메시지 크기 체크
            if (recv_buf.size() > config_.max_message_size) {
                lock.~lock_guard();
                disconnect_client(client_id, "Message too large");
                return;
            }
        }

        // 메시지 파싱
        while (true) {
            std::vector<uint8_t>* recv_buf = nullptr;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                auto buf_it = recv_buffers_.find(client_id);
                if (buf_it == recv_buffers_.end()) {
                    return;
                }
                recv_buf = &buf_it->second;

                // 헤더 크기 체크
                if (recv_buf->size() < sizeof(MessageHeader)) {
                    break;
                }

                // 헤더 파싱
                MessageHeader header;
                std::memcpy(&header, recv_buf->data(), sizeof(MessageHeader));

                // 유효성 검사
                if (!header.is_valid()) {
                    lock.~lock_guard();
                    disconnect_client(client_id, "Invalid message header");
                    return;
                }

                // 전체 메시지 크기 체크
                size_t total_size = sizeof(MessageHeader) + header.payload_length;
                if (recv_buf->size() < total_size) {
                    break;  // 더 많은 데이터 필요
                }

                // 메시지 추출
                Message msg;
                msg.header = header;
                msg.payload.assign(
                    recv_buf->begin() + sizeof(MessageHeader),
                    recv_buf->begin() + total_size
                );

                // 버퍼에서 제거
                recv_buf->erase(recv_buf->begin(), recv_buf->begin() + total_size);

                auto client_it = clients_.find(client_id);
                if (client_it != clients_.end()) {
                    client_it->second.messages_received++;
                }
                total_messages_received_.fetch_add(1, std::memory_order_relaxed);

                // 락 해제 후 메시지 처리
                lock.~lock_guard();
                process_message(client_id, msg);
            }
        }
    }
}

void TcpServer::process_message(int client_id, const Message& message) {
    MessageType type = message.header.type();

    // Pong 응답
    if (type == MessageType::Ping) {
        Message pong(MessageType::Pong);
        send_message(client_id, pong);
        return;
    }

    // 인증 요청 처리
    if (type == MessageType::AuthRequest) {
        handle_auth(client_id, message);
        return;
    }

    // 인증 필요 여부 확인
    if (config_.require_auth) {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it == clients_.end() || it->second.state != ClientState::Authenticated) {
            Message error(MessageType::Error, "Authentication required");
            send_message(client_id, error);
            return;
        }
    }

    // 메시지 콜백 호출
    MessageCallback cb;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        cb = on_message_callback_;
    }

    if (cb) {
        try {
            cb(client_id, message);
        } catch (const std::exception& e) {
            // 메시지 콜백 에러 로깅 (처리 계속)
        }
    }
}

void TcpServer::handle_auth(int client_id, const Message& message) {
    // 페이로드에서 username:password 파싱 (간단한 형식)
    std::string payload = message.payload_str();
    size_t colon_pos = payload.find(':');

    std::string username, password;
    if (colon_pos != std::string::npos) {
        username = payload.substr(0, colon_pos);
        password = payload.substr(colon_pos + 1);
    }

    bool auth_success = false;

    // 인증 콜백 호출
    AuthCallback cb;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        cb = auth_callback_;
    }

    if (cb) {
        try {
            auth_success = cb(username, password);
        } catch (...) {
            auth_success = false;
        }
    } else if (!config_.require_auth) {
        // 인증이 필요 없으면 항상 성공
        auth_success = true;
    }

    // 결과 전송
    if (auth_success) {
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
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

        // 인증 실패 시 연결 해제
        disconnect_client(client_id, "Authentication failed");
    }
}

int TcpServer::add_client(int fd, const std::string& addr, int port) {
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
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_[client_id] = info;
        fd_to_client_[fd] = client_id;
        recv_buffers_[client_id] = {};
    }

    return client_id;
}

void TcpServer::remove_client(int client_id) {
    ClientInfo info;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);

        auto it = clients_.find(client_id);
        if (it == clients_.end()) {
            return;
        }

        info = it->second;
        found = true;

        // epoll에서 제거 및 소켓 닫기
        if (info.fd >= 0) {
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, info.fd, nullptr);
            ::close(info.fd);
        }

        fd_to_client_.erase(info.fd);
        recv_buffers_.erase(client_id);
        clients_.erase(it);
    }

    if (found) {
        total_disconnections_.fetch_add(1, std::memory_order_relaxed);

        // 연결 해제 콜백
        ClientCallback cb;
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            cb = on_disconnected_callback_;
        }

        if (cb) {
            try {
                cb(client_id, info);
            } catch (const std::exception& e) {
                // 연결 해제 콜백 에러 로깅 (처리 계속)
            }
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
void TcpServer::process_message(int, const Message&) {}
void TcpServer::handle_auth(int, const Message&) {}
int TcpServer::add_client(int, const std::string&, int) { return -1; }
void TcpServer::remove_client(int) {}

#endif  // __linux__

}  // namespace arbitrage
