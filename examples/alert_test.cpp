/**
 * Alert System Test (TASK_24)
 *
 * 알림 시스템 기능 테스트
 * - Alert 구조체
 * - 포맷팅
 * - Rate limiting
 * - AlertService
 */

#include "arbitrage/ops/alert.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

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
// Mock HTTP Client
// =============================================================================

class MockHttpClient : public HttpClient {
public:
    std::atomic<int> request_count{0};
    std::atomic<int> post_count{0};
    int status_to_return{200};
    bool should_fail{false};

    Result<HttpResponse> request(const HttpRequest& req) override {
        request_count++;

        if (should_fail) {
            return Err<HttpResponse>(ErrorCode::NetworkError, "Mock network error");
        }

        HttpResponse response;
        response.status_code = status_to_return;
        response.body = R"({"ok":true})";
        return response;
    }

    Result<HttpResponse> get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}) override
    {
        HttpRequest req;
        req.method = HttpMethod::GET;
        req.url = url;
        req.headers = headers;
        return request(req);
    }

    Result<HttpResponse> post(
        const std::string& url,
        const std::string& body,
        const std::map<std::string, std::string>& headers = {}) override
    {
        post_count++;

        HttpRequest req;
        req.method = HttpMethod::POST;
        req.url = url;
        req.body = body;
        req.headers = headers;
        return request(req);
    }

    Result<HttpResponse> del(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}) override
    {
        HttpRequest req;
        req.method = HttpMethod::DELETE;
        req.url = url;
        req.headers = headers;
        return request(req);
    }
};

// =============================================================================
// 테스트 케이스
// =============================================================================

TEST(alert_level_names) {
    ASSERT(std::string(alert_level_name(AlertLevel::Info)) == "INFO");
    ASSERT(std::string(alert_level_name(AlertLevel::Warning)) == "WARNING");
    ASSERT(std::string(alert_level_name(AlertLevel::Error)) == "ERROR");
    ASSERT(std::string(alert_level_name(AlertLevel::Critical)) == "CRITICAL");
}

TEST(alert_level_emojis) {
    ASSERT(std::string(alert_level_emoji(AlertLevel::Info)).find("ℹ") != std::string::npos ||
           std::string(alert_level_emoji(AlertLevel::Info)).length() > 0);
    ASSERT(std::string(alert_level_emoji(AlertLevel::Warning)).length() > 0);
    ASSERT(std::string(alert_level_emoji(AlertLevel::Error)).length() > 0);
    ASSERT(std::string(alert_level_emoji(AlertLevel::Critical)).length() > 0);
}

TEST(alert_construction) {
    Alert alert(AlertLevel::Warning, "Test Title", "Test Message");

    ASSERT_EQ(alert.level, AlertLevel::Warning);
    ASSERT(alert.title == "Test Title");
    ASSERT(alert.message == "Test Message");
    ASSERT(alert.timestamp.time_since_epoch().count() > 0);
}

TEST(alert_format_text) {
    Alert alert(AlertLevel::Error, "Error Title", "Error occurred");
    alert.source = "TestComponent";

    std::string text = alert.format_text();

    ASSERT(text.find("ERROR") != std::string::npos);
    ASSERT(text.find("Error Title") != std::string::npos);
    ASSERT(text.find("Error occurred") != std::string::npos);
    ASSERT(text.find("TestComponent") != std::string::npos);
}

TEST(alert_format_telegram) {
    Alert alert(AlertLevel::Critical, "Critical Alert", "System down");

    std::string telegram = alert.format_telegram();

    ASSERT(telegram.find("CRITICAL") != std::string::npos);
    ASSERT(telegram.find("Critical Alert") != std::string::npos);
    ASSERT(telegram.find("System down") != std::string::npos);
    ASSERT(telegram.find("<b>") != std::string::npos);  // HTML 태그
}

TEST(alert_format_discord) {
    Alert alert(AlertLevel::Warning, "Warning Alert", "Check system");
    alert.source = "Monitor";

    std::string discord = alert.format_discord();

    ASSERT(discord.find("embeds") != std::string::npos);
    ASSERT(discord.find("Warning Alert") != std::string::npos);
    ASSERT(discord.find("Check system") != std::string::npos);
    ASSERT(discord.find("color") != std::string::npos);
}

TEST(alert_config_defaults) {
    AlertConfig config;

    ASSERT(!config.telegram.enabled);
    ASSERT(!config.discord.enabled);
    ASSERT(!config.slack.enabled);
    ASSERT(config.async_send);
    ASSERT(config.queue_size > 0);
    ASSERT(config.rate_limit.max_per_minute > 0);
}

TEST(telegram_config) {
    TelegramConfig config;
    config.enabled = true;
    config.bot_token = "123456:ABC-DEF";
    config.chat_id = "-100123456789";
    config.min_level = AlertLevel::Warning;

    ASSERT(config.enabled);
    ASSERT(config.bot_token == "123456:ABC-DEF");
    ASSERT(config.min_level == AlertLevel::Warning);
}

TEST(discord_config) {
    DiscordConfig config;
    config.enabled = true;
    config.webhook_url = "https://discord.com/api/webhooks/123/abc";
    config.username = "Test Bot";

    ASSERT(config.enabled);
    ASSERT(config.webhook_url.find("discord.com") != std::string::npos);
    ASSERT(config.username == "Test Bot");
}

TEST(alert_service_creation) {
    AlertConfig config;
    AlertService service(config);

    ASSERT(!service.is_running());
    ASSERT_EQ(service.pending_count(), 0);
}

TEST(alert_service_start_stop) {
    AlertConfig config;
    AlertService service(config);

    service.start();
    ASSERT(service.is_running());

    service.stop();
    ASSERT(!service.is_running());
}

TEST(alert_service_send_disabled) {
    AlertConfig config;
    // 모든 채널 비활성화
    config.telegram.enabled = false;
    config.discord.enabled = false;
    config.slack.enabled = false;
    config.async_send = false;

    AlertService service(config);

    Alert alert(AlertLevel::Info, "Test", "Test message");
    auto result = service.send_sync(alert);

    // 채널이 없어도 성공으로 처리
    ASSERT(result.has_value());
}

TEST(alert_service_with_mock_client) {
    AlertConfig config;
    config.telegram.enabled = true;
    config.telegram.bot_token = "test_token";
    config.telegram.chat_id = "test_chat";
    config.telegram.min_level = AlertLevel::Info;
    config.async_send = false;

    AlertService service(config);

    auto mock_client = std::make_unique<MockHttpClient>();
    auto* mock_ptr = mock_client.get();
    service.set_http_client(std::move(mock_client));

    Alert alert(AlertLevel::Info, "Test Alert", "Test message");
    auto result = service.send_sync(alert);

    ASSERT(result.has_value());
    ASSERT(mock_ptr->post_count > 0);
}

TEST(alert_service_discord_send) {
    AlertConfig config;
    config.discord.enabled = true;
    config.discord.webhook_url = "https://discord.com/api/webhooks/test";
    config.discord.min_level = AlertLevel::Info;
    config.async_send = false;

    AlertService service(config);

    auto mock_client = std::make_unique<MockHttpClient>();
    mock_client->status_to_return = 204;  // Discord returns 204
    auto* mock_ptr = mock_client.get();
    service.set_http_client(std::move(mock_client));

    Alert alert(AlertLevel::Warning, "Discord Test", "Test message");
    auto result = service.send_sync(alert);

    ASSERT(result.has_value());
    ASSERT(mock_ptr->post_count > 0);
}

TEST(alert_service_level_filter) {
    AlertConfig config;
    config.telegram.enabled = true;
    config.telegram.bot_token = "test";
    config.telegram.chat_id = "test";
    config.telegram.min_level = AlertLevel::Error;  // Error 이상만
    config.async_send = false;

    AlertService service(config);

    auto mock_client = std::make_unique<MockHttpClient>();
    auto* mock_ptr = mock_client.get();
    service.set_http_client(std::move(mock_client));

    // Info 레벨은 필터링됨
    Alert info_alert(AlertLevel::Info, "Info", "Should be filtered");
    service.send_sync(info_alert);
    ASSERT_EQ(mock_ptr->post_count.load(), 0);

    // Error 레벨은 전송됨
    Alert error_alert(AlertLevel::Error, "Error", "Should be sent");
    service.send_sync(error_alert);
    ASSERT(mock_ptr->post_count.load() > 0);
}

TEST(alert_service_rate_limit) {
    AlertConfig config;
    config.telegram.enabled = true;
    config.telegram.bot_token = "test";
    config.telegram.chat_id = "test";
    config.telegram.min_level = AlertLevel::Info;
    config.rate_limit.max_per_minute = 5;
    config.rate_limit.aggregate_similar = false;
    config.async_send = false;

    AlertService service(config);

    auto mock_client = std::make_unique<MockHttpClient>();
    service.set_http_client(std::move(mock_client));

    // 5개까지는 성공
    for (int i = 0; i < 5; ++i) {
        Alert alert(AlertLevel::Info, "Test " + std::to_string(i), "Message");
        auto result = service.send_sync(alert);
        ASSERT(result.has_value());
    }

    // 6번째는 rate limited
    Alert alert(AlertLevel::Info, "Test 6", "Message");
    auto result = service.send_sync(alert);
    ASSERT(result.has_error());
    ASSERT(result.error().code == ErrorCode::RateLimited);
}

TEST(alert_service_cooldown) {
    AlertConfig config;
    config.telegram.enabled = true;
    config.telegram.bot_token = "test";
    config.telegram.chat_id = "test";
    config.telegram.min_level = AlertLevel::Info;
    config.rate_limit.cooldown = std::chrono::seconds(2);
    config.rate_limit.aggregate_similar = true;
    config.rate_limit.max_per_minute = 100;  // 높게 설정
    config.async_send = false;

    AlertService service(config);

    auto mock_client = std::make_unique<MockHttpClient>();
    service.set_http_client(std::move(mock_client));

    // 첫 번째 알림 - 성공
    Alert alert1(AlertLevel::Info, "Same Title", "Message 1");
    auto result1 = service.send_sync(alert1);
    ASSERT(result1.has_value());

    // 같은 타이틀로 바로 다시 - 쿨다운으로 실패
    Alert alert2(AlertLevel::Info, "Same Title", "Message 2");
    auto result2 = service.send_sync(alert2);
    ASSERT(result2.has_error());
}

TEST(alert_service_async) {
    AlertConfig config;
    config.telegram.enabled = true;
    config.telegram.bot_token = "test";
    config.telegram.chat_id = "test";
    config.telegram.min_level = AlertLevel::Info;
    config.async_send = true;
    config.rate_limit.max_per_minute = 100;
    config.rate_limit.aggregate_similar = false;

    AlertService service(config);

    auto mock_client = std::make_unique<MockHttpClient>();
    auto* mock_ptr = mock_client.get();
    service.set_http_client(std::move(mock_client));

    service.start();

    // 비동기 전송
    Alert alert(AlertLevel::Info, "Async Test", "Message");
    auto future = service.send(alert);

    // 결과 대기
    auto result = future.get();
    ASSERT(result.has_value());
    ASSERT(mock_ptr->post_count > 0);

    service.stop();
}

TEST(alert_service_convenience_methods) {
    AlertConfig config;
    config.async_send = false;
    // 모든 채널 비활성화

    AlertService service(config);

    // 이 메서드들은 예외를 던지지 않아야 함
    service.info("Info Title", "Info message");
    service.warning("Warning Title", "Warning message");
    service.error("Error Title", "Error message");
    service.critical("Critical Title", "Critical message");
    service.send_from("TestSource", AlertLevel::Warning, "Source Test", "Message");

    ASSERT(true);  // 예외 없이 완료
}

TEST(alert_service_statistics) {
    AlertConfig config;
    config.telegram.enabled = true;
    config.telegram.bot_token = "test";
    config.telegram.chat_id = "test";
    config.telegram.min_level = AlertLevel::Info;
    config.async_send = false;
    config.rate_limit.max_per_minute = 100;
    config.rate_limit.aggregate_similar = false;

    AlertService service(config);

    auto mock_client = std::make_unique<MockHttpClient>();
    service.set_http_client(std::move(mock_client));

    // 몇 개 전송
    for (int i = 0; i < 3; ++i) {
        Alert alert(AlertLevel::Info, "Test " + std::to_string(i), "Message");
        service.send_sync(alert);
    }

    const auto& stats = service.stats();
    ASSERT_EQ(stats.total_sent.load(), 3);
    ASSERT(stats.telegram_sent.load() > 0);
}

TEST(alert_service_network_failure) {
    AlertConfig config;
    config.telegram.enabled = true;
    config.telegram.bot_token = "test";
    config.telegram.chat_id = "test";
    config.telegram.min_level = AlertLevel::Info;
    config.async_send = false;
    config.max_retries = 0;  // 재시도 없음

    AlertService service(config);

    auto mock_client = std::make_unique<MockHttpClient>();
    mock_client->should_fail = true;
    service.set_http_client(std::move(mock_client));

    Alert alert(AlertLevel::Error, "Test", "Message");
    auto result = service.send_sync(alert);

    ASSERT(result.has_error());

    const auto& stats = service.stats();
    ASSERT(stats.total_failed.load() > 0);
}

TEST(global_alert_functions) {
    // 글로벌 함수들이 예외 없이 동작하는지 확인
    // (실제 전송은 하지 않음 - 채널 비활성화 상태)

    AlertConfig config;
    config.async_send = false;
    alert_service().set_config(config);

    // 예외 없이 완료되어야 함
    alert_info("Global Info", "Test");
    alert_warning("Global Warning", "Test");
    alert_error("Global Error", "Test");
    alert_critical("Global Critical", "Test");

    ASSERT(true);
}

// =============================================================================
// 메인
// =============================================================================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "  Alert System Test (TASK_24)\n";
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
