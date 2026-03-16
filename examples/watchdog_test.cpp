/**
 * Watchdog Test (TASK_26)
 *
 * 워치독 기능 테스트
 * - WatchdogClient 테스트
 * - Watchdog 서비스 테스트
 * - 하트비트 처리
 * - 상태 영속화
 */

#include "arbitrage/infra/watchdog_client.hpp"
#include "arbitrage/infra/watchdog.hpp"
#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <filesystem>

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

#define ASSERT_NEAR(a, b, epsilon) \
    if (std::abs((a) - (b)) > (epsilon)) { \
        throw std::runtime_error("Assertion failed: " #a " != " #b " (within " #epsilon ")"); \
    }

// =============================================================================
// Heartbeat 테스트
// =============================================================================

TEST(heartbeat_defaults) {
    Heartbeat hb;

    ASSERT_EQ(hb.sequence, 0);
    ASSERT_EQ(hb.active_connections, 0);
    ASSERT_EQ(hb.pending_orders, 0);
    ASSERT_EQ(hb.component_status, 0);
    ASSERT_EQ(hb.error_count, 0);
    ASSERT_EQ(hb.warning_count, 0);
    ASSERT(!hb.is_healthy());
}

TEST(heartbeat_is_healthy) {
    Heartbeat hb;

    // 아무 컴포넌트도 OK가 아님
    ASSERT(!hb.is_healthy());

    // WebSocket OK
    hb.set_component(ComponentBit::WebSocket, true);
    ASSERT(!hb.is_healthy());

    // Strategy OK
    hb.set_component(ComponentBit::Strategy, true);
    ASSERT(!hb.is_healthy());

    // Executor OK - 이제 healthy
    hb.set_component(ComponentBit::Executor, true);
    ASSERT(hb.is_healthy());
}

TEST(heartbeat_component_bits) {
    Heartbeat hb;

    hb.set_component(ComponentBit::WebSocket, true);
    ASSERT(hb.is_component_ok(ComponentBit::WebSocket));
    ASSERT(!hb.is_component_ok(ComponentBit::Strategy));

    hb.set_component(ComponentBit::WebSocket, false);
    ASSERT(!hb.is_component_ok(ComponentBit::WebSocket));

    hb.set_component(ComponentBit::TcpServer, true);
    ASSERT(hb.is_component_ok(ComponentBit::TcpServer));
}

TEST(watchdog_command_enum) {
    ASSERT_EQ(static_cast<uint8_t>(WatchdogCommand::None), 0);
    ASSERT_EQ(static_cast<uint8_t>(WatchdogCommand::Shutdown), 1);
    ASSERT_EQ(static_cast<uint8_t>(WatchdogCommand::KillSwitch), 4);
}

TEST(watchdog_command_name) {
    ASSERT_EQ(std::string(watchdog_command_name(WatchdogCommand::None)), "None");
    ASSERT_EQ(std::string(watchdog_command_name(WatchdogCommand::Shutdown)), "Shutdown");
    ASSERT_EQ(std::string(watchdog_command_name(WatchdogCommand::SaveState)), "SaveState");
    ASSERT_EQ(std::string(watchdog_command_name(WatchdogCommand::KillSwitch)), "KillSwitch");
}

// =============================================================================
// WatchdogClientConfig 테스트
// =============================================================================

TEST(client_config_defaults) {
    WatchdogClientConfig config;

    ASSERT(!config.socket_path.empty());
    ASSERT_EQ(config.heartbeat_interval_ms, 1000);
    ASSERT_EQ(config.connect_timeout_ms, 5000);
    ASSERT_EQ(config.max_reconnect_attempts, 10);
    ASSERT(config.auto_reconnect);
}

// =============================================================================
// WatchdogClient 테스트
// =============================================================================

TEST(client_construction) {
    WatchdogClient client;

    ASSERT(!client.is_connected());
    ASSERT(client.is_standalone());
    ASSERT(!client.is_running());
    ASSERT_EQ(client.heartbeat_count(), 0);
}

TEST(client_with_config) {
    WatchdogClientConfig config;
    config.heartbeat_interval_ms = 500;
    config.max_reconnect_attempts = 5;

    WatchdogClient client(config);

    ASSERT(!client.is_connected());
    ASSERT_EQ(client.config().heartbeat_interval_ms, 500);
}

TEST(client_status_update) {
    WatchdogClient client;

    client.update_status(5, 10, 0x07, 2, 1);

    auto hb = client.current_heartbeat();
    ASSERT_EQ(hb.active_connections, 5);
    ASSERT_EQ(hb.pending_orders, 10);
    ASSERT_EQ(hb.component_status, 0x07);
    ASSERT_EQ(hb.error_count, 2);
    ASSERT_EQ(hb.warning_count, 1);
}

TEST(client_individual_status_update) {
    WatchdogClient client;

    client.set_active_connections(3);
    client.set_pending_orders(7);
    client.set_component_ok(ComponentBit::WebSocket, true);
    client.set_error_count(5);

    auto hb = client.current_heartbeat();
    ASSERT_EQ(hb.active_connections, 3);
    ASSERT_EQ(hb.pending_orders, 7);
    ASSERT(hb.is_component_ok(ComponentBit::WebSocket));
    ASSERT_EQ(hb.error_count, 5);
}

TEST(client_command_callback) {
    WatchdogClient client;

    std::atomic<int> callback_count{0};
    WatchdogCommand last_cmd = WatchdogCommand::None;

    client.on_command([&](WatchdogCommand cmd, const std::string&) {
        callback_count++;
        last_cmd = cmd;
    });

    // 콜백이 등록되었는지 확인
    ASSERT_EQ(callback_count, 0);
}

TEST(client_connection_callback) {
    WatchdogClient client;

    std::atomic<int> callback_count{0};

    client.on_connection_change([&](bool) {
        callback_count++;
    });

    // 콜백이 등록되었는지 확인
    ASSERT_EQ(callback_count, 0);
}

TEST(client_connect_no_server) {
    WatchdogClient client;

    // 서버가 없으므로 연결 실패
    bool connected = client.connect("/tmp/nonexistent_watchdog.sock");

    ASSERT(!connected);
    ASSERT(!client.is_connected());
    ASSERT(client.is_standalone());
}

// =============================================================================
// WatchdogConfig 테스트
// =============================================================================

TEST(watchdog_config_defaults) {
    WatchdogConfig config;

    ASSERT_EQ(config.heartbeat_interval_ms, 1000);
    ASSERT_EQ(config.heartbeat_timeout_ms, 5000);
    ASSERT_EQ(config.max_missed_heartbeats, 3);
    ASSERT_EQ(config.max_restarts, 10);
    ASSERT_EQ(config.restart_window_sec, 3600);
    ASSERT(config.restart_on_crash);
    ASSERT(config.restart_on_hang);
}

TEST(watchdog_config_resource_limits) {
    WatchdogConfig config;

    ASSERT_EQ(config.max_memory_bytes, 4ULL * 1024 * 1024 * 1024);
    ASSERT_NEAR(config.max_cpu_percent, 90.0, 0.001);
    ASSERT_EQ(config.resource_check_interval_ms, 10000);
}

// =============================================================================
// Watchdog 테스트
// =============================================================================

TEST(watchdog_construction) {
    Watchdog wd;

    ASSERT(!wd.is_running());
    ASSERT(!wd.is_main_process_running());
    ASSERT_EQ(wd.main_process_pid(), -1);
    ASSERT_EQ(wd.restart_count(), 0);
}

TEST(watchdog_with_config) {
    WatchdogConfig config;
    config.max_restarts = 5;
    config.heartbeat_timeout_ms = 3000;

    Watchdog wd(config);

    ASSERT_EQ(wd.config().max_restarts, 5);
    ASSERT_EQ(wd.config().heartbeat_timeout_ms, 3000);
}

TEST(watchdog_heartbeat_handling) {
    Watchdog wd;

    Heartbeat hb;
    hb.sequence = 100;
    hb.active_connections = 5;
    hb.component_status = 0x07;

    wd.handle_heartbeat(hb);

    auto last = wd.last_heartbeat();
    ASSERT_EQ(last.sequence, 100);
    ASSERT_EQ(last.active_connections, 5);
    ASSERT_EQ(last.component_status, 0x07);
}

TEST(watchdog_heartbeat_reset_missed_count) {
    Watchdog wd;

    // 초기에는 missed count가 0
    ASSERT_EQ(wd.missed_heartbeat_count(), 0);

    // 하트비트 처리 후에도 0
    Heartbeat hb;
    hb.sequence = 1;
    wd.handle_heartbeat(hb);

    ASSERT_EQ(wd.missed_heartbeat_count(), 0);
}

TEST(watchdog_status) {
    Watchdog wd;

    auto status = wd.get_status();

    ASSERT(!status.running);
    ASSERT(!status.main_process_running);
    ASSERT_EQ(status.main_process_pid, -1);
    ASSERT_EQ(status.restart_count, 0);
}

TEST(watchdog_restart_callback) {
    Watchdog wd;

    std::atomic<int> callback_count{0};

    wd.on_restart([&](int, int, const std::string&) {
        callback_count++;
    });

    // 콜백이 등록되었는지 확인
    ASSERT_EQ(callback_count, 0);
}

TEST(watchdog_alert_callback) {
    Watchdog wd;

    std::atomic<int> callback_count{0};
    std::string last_level;
    std::string last_message;

    wd.on_alert([&](const std::string& level, const std::string& message) {
        callback_count++;
        last_level = level;
        last_message = message;
    });

    // 콜백이 등록되었는지 확인
    ASSERT_EQ(callback_count, 0);
}

TEST(watchdog_heartbeat_callback) {
    Watchdog wd;

    std::atomic<int> callback_count{0};

    wd.on_heartbeat([&](const Heartbeat&) {
        callback_count++;
    });

    Heartbeat hb;
    wd.handle_heartbeat(hb);

    ASSERT_EQ(callback_count, 1);
}

// =============================================================================
// 상태 영속화 테스트
// =============================================================================

TEST(persisted_state_defaults) {
    PersistedState state;

    ASSERT_EQ(state.version, 1);
    ASSERT(state.open_positions.empty());
    ASSERT(state.pending_orders.empty());
    ASSERT_NEAR(state.total_pnl_today, 0.0, 0.001);
    ASSERT_EQ(state.total_trades_today, 0);
    ASSERT(!state.kill_switch_active);
}

TEST(watchdog_state_save_load) {
    // 테스트용 임시 디렉토리
    std::string test_dir = "/tmp/watchdog_test_state";
    std::filesystem::create_directories(test_dir);

    WatchdogConfig config;
    config.state_directory = test_dir;

    Watchdog wd(config);

    // 상태 저장
    PersistedState state;
    state.total_pnl_today = 12345.67;
    state.total_trades_today = 42;
    state.kill_switch_active = true;
    state.last_error = "Test error";
    state.saved_at = std::chrono::system_clock::now();

    wd.save_state(state);

    // 상태 로드
    auto loaded = wd.load_latest_state();

    ASSERT(loaded.has_value());
    ASSERT_NEAR(loaded->total_pnl_today, 12345.67, 0.01);
    ASSERT_EQ(loaded->total_trades_today, 42);
    ASSERT(loaded->kill_switch_active);
    ASSERT_EQ(loaded->last_error, "Test error");

    // 정리
    std::filesystem::remove_all(test_dir);
}

TEST(watchdog_list_snapshots) {
    std::string test_dir = "/tmp/watchdog_test_snapshots";
    std::filesystem::create_directories(test_dir);

    WatchdogConfig config;
    config.state_directory = test_dir;

    Watchdog wd(config);

    // 여러 상태 저장
    for (int i = 0; i < 5; ++i) {
        PersistedState state;
        state.total_trades_today = i;
        wd.save_state(state);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto snapshots = wd.list_state_snapshots(10);
    ASSERT(snapshots.size() >= 5);

    // 정리
    std::filesystem::remove_all(test_dir);
}

TEST(watchdog_cleanup_old_snapshots) {
    std::string test_dir = "/tmp/watchdog_test_cleanup";
    std::filesystem::create_directories(test_dir);

    WatchdogConfig config;
    config.state_directory = test_dir;

    Watchdog wd(config);

    // 여러 상태 저장
    for (int i = 0; i < 10; ++i) {
        PersistedState state;
        wd.save_state(state);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 3개만 유지
    wd.cleanup_old_snapshots(3);

    auto snapshots = wd.list_state_snapshots(100);
    ASSERT_EQ(snapshots.size(), 3);

    // 정리
    std::filesystem::remove_all(test_dir);
}

// =============================================================================
// EventBus 연동 테스트
// =============================================================================

TEST(watchdog_event_bus_integration) {
    auto bus = std::make_shared<EventBus>();
    Watchdog wd;
    wd.set_event_bus(bus);

    std::atomic<int> event_count{0};

    bus->subscribe<events::HeartbeatReceived>([&](const events::HeartbeatReceived&) {
        event_count++;
    });

    Heartbeat hb;
    hb.sequence = 1;
    wd.handle_heartbeat(hb);

    ASSERT_EQ(event_count, 1);
}

// =============================================================================
// RestartEvent 테스트
// =============================================================================

TEST(restart_event_defaults) {
    RestartEvent event;

    ASSERT_EQ(event.old_pid, 0);
    ASSERT_EQ(event.new_pid, 0);
    ASSERT(event.reason.empty());
    ASSERT_EQ(event.exit_code, 0);
}

TEST(watchdog_restart_history) {
    Watchdog wd;

    auto history = wd.get_restart_history();
    ASSERT(history.empty());
}

// =============================================================================
// 글로벌 함수 테스트
// =============================================================================

TEST(global_watchdog_client) {
    auto& client = watchdog_client();

    ASSERT(!client.is_connected());
    ASSERT(client.is_standalone());
}

TEST(global_watchdog) {
    auto& wd = watchdog();

    ASSERT(!wd.is_running());
}

// =============================================================================
// 메인
// =============================================================================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "  Watchdog Test (TASK_26)\n";
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
