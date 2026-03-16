/**
 * TCP Server Test (TASK_23)
 *
 * TCP 서버 기능 테스트
 * - 서버 시작/중지
 * - 클라이언트 연결
 * - 메시지 송수신
 * - 브로드캐스트
 * - 인증
 */

#include "arbitrage/infra/tcp_server.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace arbitrage;

// 테스트 결과 카운터
static std::atomic<int> tests_run{0};
static std::atomic<int> tests_passed{0};

#define TEST(name) \
    void test_##name(); \
    struct TestRunner_##name { \
        TestRunner_##name() { \
            std::cout << "  [TEST] " << #name << "..." << std::flush; \
            tests_run++; \
            try { \
                test_##name(); \
                tests_passed++; \
                std::cout << " PASSED" << std::endl; \
            } catch (const std::exception& e) { \
                std::cout << " FAILED: " << e.what() << std::endl; \
            } catch (...) { \
                std::cout << " FAILED: Unknown error" << std::endl; \
            } \
        } \
    } test_runner_##name; \
    void test_##name()

#define ASSERT(cond) \
    if (!(cond)) { \
        throw std::runtime_error("Assertion failed: " #cond); \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        throw std::runtime_error("Assertion failed: " #a " != " #b); \
    }

// =============================================================================
// 유틸리티
// =============================================================================

#ifdef __linux__

// 테스트용 클라이언트 소켓 생성
int create_test_client(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

// 메시지 전송
bool send_test_message(int fd, MessageType type, const std::string& payload) {
    Message msg(type, payload);
    auto data = msg.serialize();
    return ::send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size());
}

// 메시지 수신
bool recv_test_message(int fd, Message& msg, int timeout_ms = 1000) {
    // 타임아웃 설정
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 헤더 수신
    MessageHeader header;
    ssize_t n = ::recv(fd, &header, sizeof(header), MSG_WAITALL);
    if (n != sizeof(header)) return false;

    if (!header.is_valid()) return false;

    msg.header = header;

    // 페이로드 수신
    if (header.payload_length > 0) {
        msg.payload.resize(header.payload_length);
        n = ::recv(fd, msg.payload.data(), header.payload_length, MSG_WAITALL);
        if (n != static_cast<ssize_t>(header.payload_length)) return false;
    }

    return true;
}

#endif

// =============================================================================
// 테스트 케이스
// =============================================================================

TEST(message_header_size) {
    ASSERT_EQ(sizeof(MessageHeader), 8);
}

TEST(message_header_validity) {
    MessageHeader valid;
    ASSERT(valid.is_valid());

    MessageHeader invalid;
    invalid.magic[0] = 0x00;
    ASSERT(!invalid.is_valid());
}

TEST(message_serialization) {
    Message msg(MessageType::Ping, "test payload");

    auto data = msg.serialize();
    ASSERT_EQ(data.size(), sizeof(MessageHeader) + 12);

    // 헤더 확인
    MessageHeader* hdr = reinterpret_cast<MessageHeader*>(data.data());
    ASSERT(hdr->is_valid());
    ASSERT_EQ(hdr->type(), MessageType::Ping);
    ASSERT_EQ(hdr->payload_length, 12);
}

TEST(message_type_names) {
    ASSERT(std::string(message_type_name(MessageType::Ping)) == "Ping");
    ASSERT(std::string(message_type_name(MessageType::Pong)) == "Pong");
    ASSERT(std::string(message_type_name(MessageType::AuthRequest)) == "AuthRequest");
    ASSERT(std::string(message_type_name(MessageType::TickerUpdate)) == "TickerUpdate");
}

TEST(json_payload_helper) {
    auto json = make_json_payload({
        {"key1", "value1"},
        {"key2", "value2"}
    });

    ASSERT(json.find("\"key1\":\"value1\"") != std::string::npos);
    ASSERT(json.find("\"key2\":\"value2\"") != std::string::npos);
}

TEST(json_payload_with_numbers) {
    auto json = make_json_payload_num(
        {{"name", "test"}},
        {{"price", 123.45}, {"qty", 100.0}}
    );

    ASSERT(json.find("\"name\":\"test\"") != std::string::npos);
    ASSERT(json.find("\"price\":") != std::string::npos);
    ASSERT(json.find("\"qty\":") != std::string::npos);
}

TEST(server_config_defaults) {
    TcpServerConfig config;
    ASSERT_EQ(config.port, 9090);
    ASSERT_EQ(config.max_clients, 100);
    ASSERT(config.require_auth);
    ASSERT(config.bind_address == "0.0.0.0");
}

#ifdef __linux__

TEST(server_start_stop) {
    TcpServerConfig config;
    config.port = 19090;  // 테스트용 포트
    config.require_auth = false;

    TcpServer server(config);

    auto result = server.start();
    ASSERT(result.has_value());
    ASSERT(server.is_running());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    server.stop();
    ASSERT(!server.is_running());
}

TEST(server_client_count) {
    TcpServerConfig config;
    config.port = 19091;
    config.require_auth = false;

    TcpServer server(config);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ASSERT_EQ(server.client_count(), 0);

    // 클라이언트 연결
    int client = create_test_client(config.port);
    ASSERT(client >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(server.client_count(), 1);

    ::close(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(server.client_count(), 0);

    server.stop();
}

TEST(server_callbacks) {
    TcpServerConfig config;
    config.port = 19092;
    config.require_auth = false;

    TcpServer server(config);

    std::atomic<int> connected_count{0};
    std::atomic<int> disconnected_count{0};

    server.on_client_connected([&](int, const ClientInfo&) {
        connected_count++;
    });

    server.on_client_disconnected([&](int, const ClientInfo&) {
        disconnected_count++;
    });

    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = create_test_client(config.port);
    ASSERT(client >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(connected_count.load(), 1);

    ::close(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(disconnected_count.load(), 1);

    server.stop();
}

TEST(server_ping_pong) {
    TcpServerConfig config;
    config.port = 19093;
    config.require_auth = false;

    TcpServer server(config);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = create_test_client(config.port);
    ASSERT(client >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Ping 전송
    ASSERT(send_test_message(client, MessageType::Ping, ""));

    // Pong 수신
    Message response;
    ASSERT(recv_test_message(client, response));
    ASSERT_EQ(response.header.type(), MessageType::Pong);

    ::close(client);
    server.stop();
}

TEST(server_message_callback) {
    TcpServerConfig config;
    config.port = 19094;
    config.require_auth = false;

    TcpServer server(config);

    std::atomic<bool> message_received{false};
    std::string received_payload;

    server.on_message([&](int, const Message& msg) {
        if (msg.header.type() == MessageType::CmdGetStatus) {
            message_received = true;
            received_payload = msg.payload_str();
        }
    });

    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = create_test_client(config.port);
    ASSERT(client >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 명령 전송
    ASSERT(send_test_message(client, MessageType::CmdGetStatus, "status request"));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT(message_received.load());
    ASSERT(received_payload == "status request");

    ::close(client);
    server.stop();
}

TEST(server_authentication) {
    TcpServerConfig config;
    config.port = 19095;
    config.require_auth = true;

    TcpServer server(config);

    server.set_auth_callback([](const std::string& user, const std::string& pass) {
        return user == "admin" && pass == "secret123";
    });

    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 잘못된 인증
    int client1 = create_test_client(config.port);
    ASSERT(client1 >= 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ASSERT(send_test_message(client1, MessageType::AuthRequest, "wrong:password"));

    Message response1;
    ASSERT(recv_test_message(client1, response1));
    ASSERT_EQ(response1.header.type(), MessageType::AuthResponse);
    ASSERT(response1.payload_str() == "FAILED");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 올바른 인증
    int client2 = create_test_client(config.port);
    ASSERT(client2 >= 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ASSERT(send_test_message(client2, MessageType::AuthRequest, "admin:secret123"));

    Message response2;
    ASSERT(recv_test_message(client2, response2));
    ASSERT_EQ(response2.header.type(), MessageType::AuthResponse);
    ASSERT(response2.payload_str() == "OK");

    ::close(client1);
    ::close(client2);
    server.stop();
}

TEST(server_broadcast) {
    TcpServerConfig config;
    config.port = 19096;
    config.require_auth = false;

    TcpServer server(config);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 여러 클라이언트 연결
    int client1 = create_test_client(config.port);
    int client2 = create_test_client(config.port);
    ASSERT(client1 >= 0);
    ASSERT(client2 >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(server.client_count(), 2);

    // 브로드캐스트 (인증 없이도 Connected 상태에서 가능하도록 테스트 조정)
    // require_auth = false 이므로 Authenticated로 간주
    server.broadcast_if(
        Message(MessageType::TickerUpdate, "BTC:50000"),
        [](const ClientInfo&) { return true; }  // 모든 클라이언트에게
    );

    // 양쪽에서 수신 확인
    Message msg1, msg2;
    ASSERT(recv_test_message(client1, msg1));
    ASSERT(recv_test_message(client2, msg2));

    ASSERT_EQ(msg1.header.type(), MessageType::TickerUpdate);
    ASSERT_EQ(msg2.header.type(), MessageType::TickerUpdate);
    ASSERT(msg1.payload_str() == "BTC:50000");
    ASSERT(msg2.payload_str() == "BTC:50000");

    ::close(client1);
    ::close(client2);
    server.stop();
}

TEST(server_send_to_client) {
    TcpServerConfig config;
    config.port = 19097;
    config.require_auth = false;

    TcpServer server(config);

    int target_client_id = -1;

    server.on_client_connected([&](int client_id, const ClientInfo&) {
        target_client_id = client_id;
    });

    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = create_test_client(config.port);
    ASSERT(client >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT(target_client_id > 0);

    // 특정 클라이언트에 메시지 전송
    Message msg(MessageType::SystemStatus, "all systems operational");
    ASSERT(server.send_message(target_client_id, msg));

    // 수신 확인
    Message received;
    ASSERT(recv_test_message(client, received));
    ASSERT_EQ(received.header.type(), MessageType::SystemStatus);
    ASSERT(received.payload_str() == "all systems operational");

    ::close(client);
    server.stop();
}

TEST(server_statistics) {
    TcpServerConfig config;
    config.port = 19098;
    config.require_auth = false;

    TcpServer server(config);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = create_test_client(config.port);
    ASSERT(client >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Ping/Pong 몇 번
    for (int i = 0; i < 3; ++i) {
        send_test_message(client, MessageType::Ping, "");
        Message pong;
        recv_test_message(client, pong);
    }

    ::close(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto stats = server.get_stats();
    ASSERT_EQ(stats.total_connections, 1);
    ASSERT_EQ(stats.total_disconnections, 1);
    ASSERT(stats.total_messages_received >= 3);
    ASSERT(stats.total_messages_sent >= 3);

    server.stop();
}

TEST(server_get_clients) {
    TcpServerConfig config;
    config.port = 19099;
    config.require_auth = false;

    TcpServer server(config);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = create_test_client(config.port);
    ASSERT(client >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto clients = server.get_clients();
    ASSERT_EQ(clients.size(), 1);
    ASSERT(clients[0].address == "127.0.0.1");
    ASSERT(clients[0].state == ClientState::Connected);

    ::close(client);
    server.stop();
}

#else

TEST(linux_only_warning) {
    std::cout << "\n  [WARNING] TCP server tests are Linux-only. Skipping...\n";
    ASSERT(true);
}

#endif  // __linux__

// =============================================================================
// 메인
// =============================================================================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "  TCP Server Test (TASK_23)\n";
    std::cout << "========================================\n\n";

    // 테스트 실행됨 (TestRunner에 의해 자동 실행)

    std::cout << "\n========================================\n";
    std::cout << "  Test Results\n";
    std::cout << "========================================\n";
    std::cout << "  Tests run:    " << tests_run.load() << "\n";
    std::cout << "  Passed:       " << tests_passed.load() << "\n";
    std::cout << "  Failed:       " << (tests_run.load() - tests_passed.load()) << "\n";
    std::cout << "========================================\n\n";

    if (tests_passed.load() == tests_run.load()) {
        std::cout << "  ALL TESTS PASSED!\n\n";
        return 0;
    } else {
        std::cout << "  SOME TESTS FAILED!\n\n";
        return 1;
    }
}
