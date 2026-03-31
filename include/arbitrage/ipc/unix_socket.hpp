#pragma once

/**
 * TASK_42: Unix Domain Socket IPC
 *
 * Cold Path 프로세스 간 통신
 * - UnixSocketServer: epoll 기반 다중 클라이언트 처리
 * - UnixSocketClient: 연결 + 수신 스레드
 */

#include "arbitrage/ipc/ipc_protocol.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/spin_wait.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace arbitrage {

// =============================================================================
// UnixSocketServer
// =============================================================================

class UnixSocketServer {
public:
    using MessageCallback = std::function<void(int client_fd, uint8_t type,
                                                const void* data, size_t len)>;
    using ConnectCallback = std::function<void(int client_fd)>;

    explicit UnixSocketServer(const std::string& socket_path);
    ~UnixSocketServer();

    UnixSocketServer(const UnixSocketServer&) = delete;
    UnixSocketServer& operator=(const UnixSocketServer&) = delete;

    void start();
    void stop();

    void broadcast(uint8_t type, const void* data, size_t len);
    void send_to(int client_fd, uint8_t type, const void* data, size_t len);

    void on_message(MessageCallback cb);
    void on_client_connected(ConnectCallback cb);
    void on_client_disconnected(ConnectCallback cb);

    bool is_running() const { return running_.load(std::memory_order_acquire); }
    size_t client_count() const;

private:
    void event_loop();
    void handle_accept();
    void handle_client_read(int fd);
    void remove_client(int fd);
    bool set_nonblocking(int fd);
    bool send_frame(int fd, uint8_t type, const void* data, size_t len);

    std::string socket_path_;
    int server_fd_{-1};
    int epoll_fd_{-1};

    std::atomic<bool> running_{false};
    std::thread event_thread_;

    mutable SpinLock clients_mutex_;
    std::vector<int> client_fds_;
    std::unordered_map<int, std::vector<uint8_t>> recv_buffers_;

    MessageCallback on_message_cb_;
    ConnectCallback on_connect_cb_;
    ConnectCallback on_disconnect_cb_;
    std::shared_ptr<SimpleLogger> logger_;
};

// =============================================================================
// UnixSocketClient
// =============================================================================

class UnixSocketClient {
public:
    using MessageCallback = std::function<void(uint8_t type,
                                                const void* data, size_t len)>;

    UnixSocketClient();
    ~UnixSocketClient();

    UnixSocketClient(const UnixSocketClient&) = delete;
    UnixSocketClient& operator=(const UnixSocketClient&) = delete;

    bool connect(const std::string& socket_path);
    void disconnect();

    bool send(uint8_t type, const void* data, size_t len);

    void on_message(MessageCallback cb);
    void start_recv();
    void stop_recv();

    bool is_connected() const { return connected_.load(std::memory_order_acquire); }

private:
    void recv_loop();
    bool send_frame(uint8_t type, const void* data, size_t len);

    int fd_{-1};
    std::atomic<bool> connected_{false};
    std::atomic<bool> recv_running_{false};
    std::thread recv_thread_;

    std::vector<uint8_t> recv_buffer_;

    MessageCallback on_message_cb_;
    std::shared_ptr<SimpleLogger> logger_;
};

}  // namespace arbitrage
