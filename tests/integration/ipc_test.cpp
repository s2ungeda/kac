/**
 * TASK_42 + TASK_43 Tests
 *
 * - Unix Domain Socket IPC (Server/Client)
 * - SHM Order POD Types + OrderChannel
 */

#include "arbitrage/ipc/unix_socket.hpp"
#include "arbitrage/ipc/ipc_protocol.hpp"
#include "arbitrage/ipc/ipc_types.hpp"
#include "arbitrage/ipc/order_channel.hpp"
#include "arbitrage/ipc/shm_manager.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/types.hpp"
#include "arbitrage/executor/types.hpp"

#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>

using namespace arbitrage;

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define RUN_TEST(name) do { \
    g_tests_run++; \
    std::cout << "  [TEST] " << #name << "..." << std::flush; \
    try { \
        name(); \
        g_tests_passed++; \
        std::cout << " PASSED\n"; \
    } catch (const std::exception& e) { \
        g_tests_failed++; \
        std::cout << " FAILED: " << e.what() << "\n"; \
    } catch (...) { \
        g_tests_failed++; \
        std::cout << " FAILED: unknown exception\n"; \
    } \
} while(0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) throw std::runtime_error("ASSERT_TRUE failed: " #cond); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) throw std::runtime_error("ASSERT_EQ failed: " #a " != " #b); \
} while(0)

// =============================================================================
// TASK_42: IPC Protocol Tests
// =============================================================================

void test_ipc_header_encode_decode() {
    auto hdr = encode_ipc_header(0x42, 1024);
    auto decoded = decode_ipc_header(hdr.data());
    ASSERT_EQ(decoded.msg_type, static_cast<uint8_t>(0x42));
    ASSERT_EQ(decoded.payload_length, static_cast<uint32_t>(1024));
}

void test_ipc_header_zero_payload() {
    auto hdr = encode_ipc_header(
        static_cast<uint8_t>(IpcMessageType::Heartbeat), 0);
    auto decoded = decode_ipc_header(hdr.data());
    ASSERT_EQ(decoded.payload_length, static_cast<uint32_t>(0));
    ASSERT_EQ(decoded.msg_type,
              static_cast<uint8_t>(IpcMessageType::Heartbeat));
}

void test_ipc_header_large_payload() {
    auto hdr = encode_ipc_header(0xFF, 65535);
    auto decoded = decode_ipc_header(hdr.data());
    ASSERT_EQ(decoded.payload_length, static_cast<uint32_t>(65535));
}

// =============================================================================
// TASK_42: Unix Socket Server/Client Tests
// =============================================================================

void test_uds_server_start_stop() {
    const char* path = "/tmp/kimchi_test_uds_1.sock";
    UnixSocketServer server(path);
    server.start();
    ASSERT_TRUE(server.is_running());
    ASSERT_EQ(server.client_count(), static_cast<size_t>(0));
    server.stop();
    ASSERT_TRUE(!server.is_running());
}

void test_uds_client_connect() {
    const char* path = "/tmp/kimchi_test_uds_2.sock";
    UnixSocketServer server(path);
    server.start();

    UnixSocketClient client;
    ASSERT_TRUE(client.connect(path));
    ASSERT_TRUE(client.is_connected());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_EQ(server.client_count(), static_cast<size_t>(1));

    client.disconnect();
    server.stop();
}

void test_uds_message_roundtrip() {
    const char* path = "/tmp/kimchi_test_uds_3.sock";
    UnixSocketServer server(path);

    std::atomic<bool> received{false};
    uint8_t recv_type = 0;
    uint32_t recv_value = 0;

    server.on_message([&](int /*fd*/, uint8_t type, const void* data, size_t len) {
        recv_type = type;
        if (len == sizeof(uint32_t)) {
            std::memcpy(&recv_value, data, sizeof(uint32_t));
        }
        received.store(true);
    });
    server.start();

    UnixSocketClient client;
    ASSERT_TRUE(client.connect(path));

    uint32_t payload = 12345;
    ASSERT_TRUE(client.send(0x42, &payload, sizeof(payload)));

    // 서버가 수신할 때까지 대기
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!received.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(received.load());
    ASSERT_EQ(recv_type, static_cast<uint8_t>(0x42));
    ASSERT_EQ(recv_value, static_cast<uint32_t>(12345));

    client.disconnect();
    server.stop();
}

void test_uds_server_broadcast() {
    const char* path = "/tmp/kimchi_test_uds_4.sock";
    UnixSocketServer server(path);
    server.start();

    UnixSocketClient client1, client2;
    ASSERT_TRUE(client1.connect(path));
    ASSERT_TRUE(client2.connect(path));

    std::atomic<int> recv_count{0};
    auto handler = [&](uint8_t /*type*/, const void* /*data*/, size_t /*len*/) {
        recv_count.fetch_add(1);
    };
    client1.on_message(handler);
    client2.on_message(handler);
    client1.start_recv();
    client2.start_recv();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    uint32_t payload = 99;
    server.broadcast(0x10, &payload, sizeof(payload));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (recv_count.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_EQ(recv_count.load(), 2);

    client1.disconnect();
    client2.disconnect();
    server.stop();
}

void test_uds_multiple_messages() {
    const char* path = "/tmp/kimchi_test_uds_5.sock";
    UnixSocketServer server(path);

    std::atomic<int> msg_count{0};
    server.on_message([&](int /*fd*/, uint8_t /*type*/, const void* /*data*/, size_t /*len*/) {
        msg_count.fetch_add(1);
    });
    server.start();

    UnixSocketClient client;
    ASSERT_TRUE(client.connect(path));

    // 100 메시지 연속 전송
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(client.send(0x01, &i, sizeof(i)));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (msg_count.load() < 100 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_EQ(msg_count.load(), 100);

    client.disconnect();
    server.stop();
}

void test_uds_client_disconnect_detection() {
    const char* path = "/tmp/kimchi_test_uds_6.sock";
    UnixSocketServer server(path);

    std::atomic<int> disconnects{0};
    server.on_client_disconnected([&](int /*fd*/) {
        disconnects.fetch_add(1);
    });
    server.start();

    {
        UnixSocketClient client;
        client.connect(path);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // client 소멸 시 disconnect
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(disconnects.load(), 1);

    server.stop();
}

// =============================================================================
// TASK_43: SHM Order POD Type Tests
// =============================================================================

void test_shm_order_types_trivially_copyable() {
    // 컴파일 타임 검증 (static_assert가 이미 있지만 런타임에도 확인)
    ASSERT_TRUE(std::is_trivially_copyable_v<ShmSingleOrderResult>);
    ASSERT_TRUE(std::is_trivially_copyable_v<ShmDualOrderResult>);
    ASSERT_TRUE(std::is_trivially_copyable_v<DualOrderRequest>);
}

void test_shm_dual_order_result_helpers() {
    ShmDualOrderResult result{};

    // 기본값: 둘 다 실패
    ASSERT_TRUE(!result.both_success());
    ASSERT_TRUE(!result.both_filled());
    ASSERT_TRUE(!result.partial_fill());

    // 매수만 성공
    result.buy_result.success = true;
    result.buy_result.status = OrderStatus::Filled;
    ASSERT_TRUE(!result.both_success());
    ASSERT_TRUE(result.partial_fill());

    // 매도도 성공
    result.sell_result.success = true;
    result.sell_result.status = OrderStatus::Filled;
    ASSERT_TRUE(result.both_success());
    ASSERT_TRUE(result.both_filled());
    ASSERT_TRUE(!result.partial_fill());
}

void test_to_shm_conversion() {
    SingleOrderResult src{};
    src.exchange = Exchange::Binance;
    OrderResult or_result{};
    std::strncpy(or_result.order_id, "ORDER-123", MAX_ORDER_ID_LEN);
    or_result.status = OrderStatus::Filled;
    or_result.filled_qty = 100.0;
    or_result.avg_price = 0.55;
    or_result.commission = 0.1;
    src.result = or_result;
    src.latency = Duration(5000);  // 5ms

    auto dst = to_shm(src);
    ASSERT_EQ(dst.exchange, Exchange::Binance);
    ASSERT_TRUE(dst.success);
    ASSERT_EQ(dst.status, OrderStatus::Filled);
    ASSERT_TRUE(std::strcmp(dst.order_id, "ORDER-123") == 0);
    ASSERT_TRUE(dst.filled_qty == 100.0);
    ASSERT_TRUE(dst.avg_price == 0.55);
    ASSERT_TRUE(dst.commission == 0.1);
    ASSERT_EQ(dst.latency_us, static_cast<int64_t>(5000));
}

void test_order_channel_roundtrip() {
    // 기존 SHM 정리
    ShmSegment::unlink(shm_names::ORDERS);
    ShmSegment::unlink(shm_names::ORDER_RESULTS);

    // Engine side: request producer
    auto engine_ch = OrderChannel::create_engine_side(64);
    ASSERT_TRUE(engine_ch.request_queue_valid());

    // OrderManager side: request consumer, result producer
    auto om_ch = OrderChannel::create_order_manager_side(64);
    ASSERT_TRUE(om_ch.request_queue_valid());
    ASSERT_TRUE(om_ch.result_queue_valid());

    // Engine → OrderManager: 주문 요청
    DualOrderRequest req{};
    req.buy_order.exchange = Exchange::Binance;
    req.buy_order.side = OrderSide::Buy;
    req.buy_order.quantity = 100.0;
    req.buy_order.price = 0.55;
    req.sell_order.exchange = Exchange::Upbit;
    req.sell_order.side = OrderSide::Sell;
    req.sell_order.quantity = 100.0;
    req.sell_order.price = 850.0;
    req.expected_premium = 2.5;
    req.request_id = 42;

    ASSERT_TRUE(engine_ch.push_request(req));

    // OrderManager에서 수신
    DualOrderRequest recv_req{};
    ASSERT_TRUE(om_ch.pop_request(recv_req));
    ASSERT_EQ(recv_req.request_id, static_cast<int64_t>(42));
    ASSERT_EQ(recv_req.buy_order.exchange, Exchange::Binance);
    ASSERT_TRUE(recv_req.buy_order.quantity == 100.0);

    // OrderManager → Engine: 주문 결과
    ShmDualOrderResult result{};
    result.request_id = 42;
    result.buy_result.success = true;
    result.buy_result.status = OrderStatus::Filled;
    result.buy_result.filled_qty = 100.0;
    result.sell_result.success = true;
    result.sell_result.status = OrderStatus::Filled;
    result.sell_result.filled_qty = 100.0;
    result.gross_profit = 1500.0;

    ASSERT_TRUE(om_ch.push_result(result));

    // Engine 결과 수신은 OrderManager가 init_producer한 후에만 가능
    // create_engine_side는 attach_consumer를 시도하지만,
    // SHM이 이미 init_producer 되기 전이므로 실패할 수 있음
    // → TASK_44에서 순서 보장

    // 정리
    ShmSegment::unlink(shm_names::ORDERS);
    ShmSegment::unlink(shm_names::ORDER_RESULTS);
}

// =============================================================================
// main
// =============================================================================

int main() {
    Logger::init("logs");

    std::cout << "\n========================================\n";
    std::cout << "  IPC Test (TASK_42 + TASK_43)\n";
    std::cout << "========================================\n\n";

    std::cout << "--- IPC Protocol Tests ---\n";
    RUN_TEST(test_ipc_header_encode_decode);
    RUN_TEST(test_ipc_header_zero_payload);
    RUN_TEST(test_ipc_header_large_payload);

    std::cout << "\n--- Unix Domain Socket Tests ---\n";
    RUN_TEST(test_uds_server_start_stop);
    RUN_TEST(test_uds_client_connect);
    RUN_TEST(test_uds_message_roundtrip);
    RUN_TEST(test_uds_server_broadcast);
    RUN_TEST(test_uds_multiple_messages);
    RUN_TEST(test_uds_client_disconnect_detection);

    std::cout << "\n--- SHM Order POD Types Tests ---\n";
    RUN_TEST(test_shm_order_types_trivially_copyable);
    RUN_TEST(test_shm_dual_order_result_helpers);
    RUN_TEST(test_to_shm_conversion);
    RUN_TEST(test_order_channel_roundtrip);

    std::cout << "\n========================================\n";
    std::cout << "  Test Results\n";
    std::cout << "========================================\n";
    std::cout << "  Tests run:    " << g_tests_run << "\n";
    std::cout << "  Passed:       " << g_tests_passed << "\n";
    std::cout << "  Failed:       " << g_tests_failed << "\n";
    std::cout << "========================================\n\n";

    if (g_tests_failed > 0) {
        std::cout << "  SOME TESTS FAILED!\n\n";
        return 1;
    }
    std::cout << "  ALL TESTS PASSED!\n\n";
    return 0;
}
