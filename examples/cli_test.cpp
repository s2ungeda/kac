/**
 * CLI Tool Test (TASK_27)
 *
 * CLI 도구 기능 테스트
 * - 설정 기본값
 * - 응답 구조체
 * - 포맷팅 함수
 * - 연결 테스트
 */

#include "../tools/cli/commands.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

using namespace arbitrage::cli;

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
// CLIConfig 테스트
// =============================================================================

TEST(config_defaults) {
    CLIConfig config;

    ASSERT_EQ(config.server_host, "localhost");
    ASSERT_EQ(config.server_port, 9800);
    ASSERT_EQ(config.connect_timeout_ms, 5000);
    ASSERT_EQ(config.read_timeout_ms, 10000);
    ASSERT(config.auth_token.empty());
    ASSERT(!config.verbose);
    ASSERT(config.color_output);
}

TEST(config_custom) {
    CLIConfig config;
    config.server_host = "192.168.1.100";
    config.server_port = 8080;
    config.auth_token = "secret123";
    config.verbose = true;

    ASSERT_EQ(config.server_host, "192.168.1.100");
    ASSERT_EQ(config.server_port, 8080);
    ASSERT_EQ(config.auth_token, "secret123");
    ASSERT(config.verbose);
}

// =============================================================================
// 응답 구조체 테스트
// =============================================================================

TEST(system_status_response) {
    SystemStatusResponse status;

    ASSERT(!status.running);
    ASSERT_EQ(status.active_connections, 0);
    ASSERT_EQ(status.pending_orders, 0);
    ASSERT(!status.kill_switch_active);
    ASSERT_NEAR(status.daily_pnl, 0.0, 0.001);
    ASSERT_EQ(status.daily_trades, 0);
}

TEST(premium_response) {
    PremiumResponse premium;

    ASSERT(premium.matrix.empty());
    ASSERT_NEAR(premium.fx_rate, 0.0, 0.001);
}

TEST(premium_entry) {
    PremiumResponse::Entry entry;
    entry.buy_exchange = "Binance";
    entry.sell_exchange = "Upbit";
    entry.premium_pct = 2.5;
    entry.buy_price = 2.15;
    entry.sell_price = 3200;
    entry.currency = "KRW";

    ASSERT_EQ(entry.buy_exchange, "Binance");
    ASSERT_EQ(entry.sell_exchange, "Upbit");
    ASSERT_NEAR(entry.premium_pct, 2.5, 0.001);
}

TEST(balance_response) {
    BalanceResponse balance;

    ASSERT(balance.balances.empty());
    ASSERT_NEAR(balance.total_value_krw, 0.0, 0.001);
}

TEST(exchange_balance) {
    BalanceResponse::ExchangeBalance eb;
    eb.exchange = "Upbit";
    eb.xrp = 1000.0;
    eb.krw = 5000000.0;

    ASSERT_EQ(eb.exchange, "Upbit");
    ASSERT_NEAR(eb.xrp, 1000.0, 0.001);
    ASSERT_NEAR(eb.krw, 5000000.0, 0.001);
}

TEST(order_status_response) {
    OrderStatusResponse order;

    ASSERT(order.order_id.empty());
    ASSERT(order.exchange.empty());
    ASSERT_NEAR(order.quantity, 0.0, 0.001);
    ASSERT_NEAR(order.filled_qty, 0.0, 0.001);
}

TEST(trade_history_response) {
    TradeHistoryResponse history;

    ASSERT(history.trades.empty());
    ASSERT_NEAR(history.total_pnl, 0.0, 0.001);
    ASSERT_EQ(history.total_count, 0);
}

TEST(health_response) {
    HealthResponse health;

    ASSERT(health.components.empty());
    ASSERT(health.overall.empty());
    ASSERT_NEAR(health.cpu_percent, 0.0, 0.001);
    ASSERT_EQ(health.memory_bytes, 0);
}

TEST(command_response) {
    CommandResponse response;

    ASSERT(!response.success);
    ASSERT(response.message.empty());
    ASSERT(response.error.empty());
}

// =============================================================================
// CLI 클래스 테스트
// =============================================================================

TEST(cli_construction) {
    CLI cli;

    ASSERT(!cli.is_connected());
    ASSERT(cli.last_error().empty());
}

TEST(cli_with_config) {
    CLIConfig config;
    config.server_host = "127.0.0.1";
    config.server_port = 9999;

    CLI cli(config);

    ASSERT(!cli.is_connected());
}

TEST(cli_connect_no_server) {
    CLIConfig config;
    config.server_host = "localhost";
    config.server_port = 59999;  // 사용되지 않는 포트
    config.connect_timeout_ms = 1000;

    CLI cli(config);

    // 서버가 없으므로 연결 실패
    bool connected = cli.connect();
    ASSERT(!connected);
    ASSERT(!cli.is_connected());
    ASSERT(!cli.last_error().empty());
}

TEST(cli_disconnect) {
    CLI cli;

    cli.disconnect();  // 연결되지 않은 상태에서 호출해도 안전
    ASSERT(!cli.is_connected());
}

TEST(cli_set_config) {
    CLI cli;

    CLIConfig config;
    config.server_port = 8888;
    config.verbose = true;

    cli.set_config(config);
    // 설정이 적용되었는지 직접 확인은 어려우나 크래시 없이 완료
    ASSERT(true);
}

// =============================================================================
// 포맷팅 함수 테스트
// =============================================================================

TEST(format_number_simple) {
    std::string result = format_number(1234.56, 2);
    ASSERT_EQ(result, "1,234.56");
}

TEST(format_number_large) {
    std::string result = format_number(1234567.89, 2);
    ASSERT_EQ(result, "1,234,567.89");
}

TEST(format_number_small) {
    std::string result = format_number(123.45, 2);
    ASSERT_EQ(result, "123.45");
}

TEST(format_number_negative) {
    std::string result = format_number(-1234.56, 2);
    ASSERT_EQ(result, "-1,234.56");
}

TEST(format_number_zero_precision) {
    std::string result = format_number(1234567.0, 0);
    ASSERT_EQ(result, "1,234,567");
}

TEST(format_krw) {
    std::string result = format_krw(5000000.0);
    ASSERT_EQ(result, "5,000,000 KRW");
}

TEST(format_percent) {
    std::string result = format_percent(2.5);
    ASSERT_EQ(result, "2.50%");
}

TEST(format_percent_negative) {
    std::string result = format_percent(-1.25);
    ASSERT_EQ(result, "-1.25%");
}

TEST(format_relative_time_seconds) {
    auto now = std::chrono::system_clock::now();
    auto past = now - std::chrono::seconds(30);
    std::string result = format_relative_time(past);
    ASSERT(result.find("30s ago") != std::string::npos);
}

TEST(format_relative_time_minutes) {
    auto now = std::chrono::system_clock::now();
    auto past = now - std::chrono::minutes(5);
    std::string result = format_relative_time(past);
    ASSERT(result.find("5m ago") != std::string::npos);
}

TEST(format_relative_time_hours) {
    auto now = std::chrono::system_clock::now();
    auto past = now - std::chrono::hours(3);
    std::string result = format_relative_time(past);
    ASSERT(result.find("3h ago") != std::string::npos);
}

TEST(format_relative_time_days) {
    auto now = std::chrono::system_clock::now();
    auto past = now - std::chrono::hours(48);
    std::string result = format_relative_time(past);
    ASSERT(result.find("2d ago") != std::string::npos);
}

// =============================================================================
// 명령 응답 테스트
// =============================================================================

TEST(command_response_success) {
    CommandResponse resp;
    resp.success = true;
    resp.message = "Operation completed";

    ASSERT(resp.success);
    ASSERT_EQ(resp.message, "Operation completed");
    ASSERT(resp.error.empty());
}

TEST(command_response_failure) {
    CommandResponse resp;
    resp.success = false;
    resp.error = "Connection refused";

    ASSERT(!resp.success);
    ASSERT(resp.message.empty());
    ASSERT_EQ(resp.error, "Connection refused");
}

// =============================================================================
// 출력 함수 테스트 (실제 출력은 생략)
// =============================================================================

TEST(print_functions_no_crash) {
    CLI cli;

    // 모든 출력 함수가 크래시 없이 실행되는지 확인
    SystemStatusResponse status;
    status.running = true;
    status.uptime = "2h 30m";
    cli.print_status(status);

    PremiumResponse premium;
    premium.fx_rate = 1450.0;
    cli.print_premium(premium);

    BalanceResponse balance;
    cli.print_balance(balance);

    TradeHistoryResponse history;
    cli.print_history(history);

    HealthResponse health;
    health.overall = "Healthy";
    cli.print_health(health);

    CommandResponse resp;
    resp.success = true;
    resp.message = "OK";
    cli.print_response(resp);

    ASSERT(true);  // 크래시 없이 완료
}

// =============================================================================
// 메인
// =============================================================================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "  CLI Tool Test (TASK_27)\n";
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
