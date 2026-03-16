/**
 * Health Check Test (TASK_22)
 *
 * 테스트 항목:
 * 1. 컴포넌트 체크 등록/해제
 * 2. 상태 조회
 * 3. 리소스 모니터링
 * 4. 알림 콜백
 * 5. RAII HealthCheckGuard
 * 6. 편의 함수
 */

#include "arbitrage/infra/health_check.hpp"
#include "arbitrage/infra/event_bus.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

using namespace arbitrage;
using namespace std::chrono_literals;

// 테스트 카운터
static int g_test_passed = 0;
static int g_test_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  [FAIL] " << msg << "\n"; \
            ++g_test_failed; \
            return false; \
        } \
    } while(0)

#define TEST_PASS(msg) \
    do { \
        std::cout << "  [PASS] " << msg << "\n"; \
        ++g_test_passed; \
    } while(0)

// =============================================================================
// 테스트 1: 상태 이름
// =============================================================================
bool test_status_names() {
    std::cout << "\n=== Test 1: Status Names ===\n";

    TEST_ASSERT(std::string(health_status_name(HealthStatus::Healthy)) == "Healthy",
                "Healthy name");
    TEST_PASS("Healthy name correct");

    TEST_ASSERT(std::string(health_status_name(HealthStatus::Degraded)) == "Degraded",
                "Degraded name");
    TEST_PASS("Degraded name correct");

    TEST_ASSERT(std::string(health_status_name(HealthStatus::Unhealthy)) == "Unhealthy",
                "Unhealthy name");
    TEST_PASS("Unhealthy name correct");

    return true;
}

// =============================================================================
// 테스트 2: 기본 체크 등록 (비싱글톤)
// =============================================================================
bool test_basic_registration() {
    std::cout << "\n=== Test 2: Basic Check Registration ===\n";

    HealthCheckerConfig config;
    HealthChecker checker(config);

    // 체크 등록
    checker.register_check("test_component", []() -> ComponentHealth {
        return ComponentHealth("test_component", HealthStatus::Healthy, "All good");
    });

    TEST_ASSERT(checker.check_count() == 1, "Check count should be 1");
    TEST_PASS("Check registered");

    // 체크 해제
    checker.unregister_check("test_component");
    TEST_ASSERT(checker.check_count() == 0, "Check count should be 0");
    TEST_PASS("Check unregistered");

    return true;
}

// =============================================================================
// 테스트 3: 상태 조회
// =============================================================================
bool test_check_all() {
    std::cout << "\n=== Test 3: Check All ===\n";

    HealthCheckerConfig config;
    config.enable_resource_monitoring = false;  // 리소스 모니터링 비활성화
    HealthChecker checker(config);

    // 테스트 컴포넌트 등록
    checker.register_check("healthy", []() {
        return ComponentHealth("healthy", HealthStatus::Healthy, "OK");
    });
    checker.register_check("degraded", []() {
        return ComponentHealth("degraded", HealthStatus::Degraded, "Slow");
    });

    // 전체 상태 조회
    auto health = checker.check_all();

    TEST_ASSERT(health.components.size() == 2, "Should have 2 components");
    TEST_PASS("Components checked");

    TEST_ASSERT(health.healthy_count == 1, "Should have 1 healthy");
    TEST_PASS("Healthy count correct");

    TEST_ASSERT(health.degraded_count == 1, "Should have 1 degraded");
    TEST_PASS("Degraded count correct");

    TEST_ASSERT(health.overall == HealthStatus::Degraded, "Overall should be Degraded");
    TEST_PASS("Overall status correct");

    return true;
}

// =============================================================================
// 테스트 4: 개별 컴포넌트 조회
// =============================================================================
bool test_check_component() {
    std::cout << "\n=== Test 4: Check Component ===\n";

    HealthCheckerConfig config;
    HealthChecker checker(config);

    checker.register_check("single", []() {
        return ComponentHealth("single", HealthStatus::Healthy, "Single check");
    });

    // 개별 조회
    auto health = checker.check_component("single");
    TEST_ASSERT(health.status == HealthStatus::Healthy, "Should be healthy");
    TEST_ASSERT(health.name == "single", "Name should match");
    TEST_PASS("Single component check works");

    // 없는 컴포넌트 조회
    auto not_found = checker.check_component("nonexistent");
    TEST_ASSERT(not_found.status == HealthStatus::Unhealthy, "Should be unhealthy");
    TEST_PASS("Nonexistent component returns unhealthy");

    return true;
}

// =============================================================================
// 테스트 5: 리소스 모니터링
// =============================================================================
bool test_resource_monitoring() {
    std::cout << "\n=== Test 5: Resource Monitoring ===\n";

    HealthCheckerConfig config;
    config.enable_resource_monitoring = true;
    HealthChecker checker(config);

    auto health = checker.check_all();

    std::cout << "    CPU: " << health.resources.cpu_percent << "%\n";
    std::cout << "    Memory: " << (health.resources.memory_used_bytes / 1024 / 1024) << " MB\n";
    std::cout << "    Memory %: " << health.resources.memory_percent << "%\n";
    std::cout << "    Open FDs: " << health.resources.open_fd_count << "\n";
    std::cout << "    Uptime: " << health.resources.uptime.count() << "s\n";

    TEST_ASSERT(health.resources.memory_used_bytes > 0, "Memory should be > 0");
    TEST_PASS("Memory usage measured");

    TEST_ASSERT(health.resources.open_fd_count > 0, "Should have open FDs");
    TEST_PASS("Open FDs counted");

    return true;
}

// =============================================================================
// 테스트 6: 알림 콜백
// =============================================================================
bool test_alert_callbacks() {
    std::cout << "\n=== Test 6: Alert Callbacks ===\n";

    HealthCheckerConfig config;
    config.enable_resource_monitoring = false;
    HealthChecker checker(config);

    std::atomic<int> unhealthy_alerts{0};
    std::atomic<int> status_changes{0};

    checker.on_unhealthy([&](const ComponentHealth&) {
        ++unhealthy_alerts;
    });

    checker.on_status_change([&](const SystemHealth&) {
        ++status_changes;
    });

    // Unhealthy 컴포넌트 등록
    checker.register_check("failing", []() {
        return ComponentHealth("failing", HealthStatus::Unhealthy, "Test failure");
    });

    // 체크 실행
    checker.check_all();

    TEST_ASSERT(unhealthy_alerts > 0, "Should have unhealthy alerts");
    TEST_PASS("Unhealthy callback fired");

    TEST_ASSERT(status_changes > 0, "Should have status change callback");
    TEST_PASS("Status change callback fired");

    return true;
}

// =============================================================================
// 테스트 7: 설정
// =============================================================================
bool test_config() {
    std::cout << "\n=== Test 7: Config ===\n";

    HealthCheckerConfig config;
    config.check_interval = std::chrono::seconds(60);
    config.cpu_warning_threshold = 90.0;
    config.memory_warning_threshold = 90.0;

    HealthChecker checker(config);
    auto loaded = checker.config();

    TEST_ASSERT(loaded.check_interval == std::chrono::seconds(60),
                "Check interval should be 60s");
    TEST_PASS("Config loaded correctly");

    return true;
}

// =============================================================================
// 테스트 8: 전체 상태 결정
// =============================================================================
bool test_overall_status() {
    std::cout << "\n=== Test 8: Overall Status ===\n";

    HealthCheckerConfig config;
    config.enable_resource_monitoring = false;
    HealthChecker checker(config);

    // 모든 Healthy
    checker.register_check("h1", []() {
        return ComponentHealth("h1", HealthStatus::Healthy);
    });
    checker.register_check("h2", []() {
        return ComponentHealth("h2", HealthStatus::Healthy);
    });

    auto health1 = checker.check_all();
    TEST_ASSERT(health1.overall == HealthStatus::Healthy,
                "All healthy should result in Healthy overall");
    TEST_PASS("All healthy = Healthy overall");

    // Unhealthy 추가
    checker.register_check("u1", []() {
        return ComponentHealth("u1", HealthStatus::Unhealthy, "Test");
    });

    auto health2 = checker.check_all();
    TEST_ASSERT(health2.overall == HealthStatus::Unhealthy,
                "Any unhealthy should result in Unhealthy overall");
    TEST_PASS("Any unhealthy = Unhealthy overall");

    return true;
}

// =============================================================================
// 테스트 9: make_simple_check 편의 함수
// =============================================================================
bool test_make_simple_check() {
    std::cout << "\n=== Test 9: make_simple_check ===\n";

    HealthCheckerConfig config;
    HealthChecker checker(config);

    // 성공하는 체크
    checker.register_check("success",
        make_simple_check("success", []() { return true; }, "Success", "Failure"));

    auto health1 = checker.check_component("success");
    TEST_ASSERT(health1.status == HealthStatus::Healthy, "Should be healthy");
    TEST_ASSERT(health1.message == "Success", "Message should be Success");
    TEST_PASS("Simple success check works");

    // 실패하는 체크
    checker.register_check("fail",
        make_simple_check("fail", []() { return false; }, "Success", "Failure"));

    auto health2 = checker.check_component("fail");
    TEST_ASSERT(health2.status == HealthStatus::Unhealthy, "Should be unhealthy");
    TEST_ASSERT(health2.message == "Failure", "Message should be Failure");
    TEST_PASS("Simple failure check works");

    return true;
}

// =============================================================================
// 테스트 10: make_conditional_check 편의 함수
// =============================================================================
bool test_make_conditional_check() {
    std::cout << "\n=== Test 10: make_conditional_check ===\n";

    HealthCheckerConfig config;
    HealthChecker checker(config);

    int state = 0;  // 0=Healthy, 1=Degraded, 2=Unhealthy

    checker.register_check("cond",
        make_conditional_check("cond",
            [&]() -> HealthStatus {
                switch (state) {
                    case 0: return HealthStatus::Healthy;
                    case 1: return HealthStatus::Degraded;
                    default: return HealthStatus::Unhealthy;
                }
            },
            [&]() -> std::string {
                switch (state) {
                    case 0: return "All good";
                    case 1: return "Slow";
                    default: return "Failed";
                }
            }));

    // Healthy
    state = 0;
    auto h1 = checker.check_component("cond");
    TEST_ASSERT(h1.status == HealthStatus::Healthy, "Should be Healthy");
    TEST_PASS("Conditional check Healthy");

    // Degraded
    state = 1;
    auto h2 = checker.check_component("cond");
    TEST_ASSERT(h2.status == HealthStatus::Degraded, "Should be Degraded");
    TEST_PASS("Conditional check Degraded");

    // Unhealthy
    state = 2;
    auto h3 = checker.check_component("cond");
    TEST_ASSERT(h3.status == HealthStatus::Unhealthy, "Should be Unhealthy");
    TEST_PASS("Conditional check Unhealthy");

    return true;
}

// =============================================================================
// 테스트 11: 히스토리
// =============================================================================
bool test_history() {
    std::cout << "\n=== Test 11: History ===\n";

    HealthCheckerConfig config;
    config.enable_resource_monitoring = false;
    HealthChecker checker(config);

    // 여러 번 체크
    checker.check_all();
    checker.check_all();
    checker.check_all();

    auto history = checker.get_history();
    TEST_ASSERT(history.size() == 3, "Should have 3 history entries");
    TEST_PASS("History stored");

    std::cout << "    History size: " << history.size() << "\n";

    return true;
}

// =============================================================================
// 테스트 12: health_ratio
// =============================================================================
bool test_health_ratio() {
    std::cout << "\n=== Test 12: Health Ratio ===\n";

    SystemHealth health;
    health.healthy_count = 3;
    health.degraded_count = 1;
    health.unhealthy_count = 1;
    health.components.resize(5);

    double ratio = health.health_ratio();
    TEST_ASSERT(ratio >= 0.59 && ratio <= 0.61, "Ratio should be ~0.6");
    TEST_PASS("Health ratio calculated correctly");

    // 빈 컴포넌트
    SystemHealth empty;
    TEST_ASSERT(empty.health_ratio() == 1.0, "Empty should return 1.0");
    TEST_PASS("Empty health ratio is 1.0");

    return true;
}

// =============================================================================
// 테스트 13: ComponentHealth 헬퍼
// =============================================================================
bool test_component_health_helpers() {
    std::cout << "\n=== Test 13: ComponentHealth Helpers ===\n";

    ComponentHealth healthy("h", HealthStatus::Healthy);
    TEST_ASSERT(healthy.is_healthy(), "is_healthy should be true");
    TEST_ASSERT(!healthy.is_degraded(), "is_degraded should be false");
    TEST_ASSERT(!healthy.is_unhealthy(), "is_unhealthy should be false");
    TEST_PASS("Healthy helpers work");

    ComponentHealth degraded("d", HealthStatus::Degraded);
    TEST_ASSERT(!degraded.is_healthy(), "is_healthy should be false");
    TEST_ASSERT(degraded.is_degraded(), "is_degraded should be true");
    TEST_ASSERT(!degraded.is_unhealthy(), "is_unhealthy should be false");
    TEST_PASS("Degraded helpers work");

    ComponentHealth unhealthy("u", HealthStatus::Unhealthy);
    TEST_ASSERT(!unhealthy.is_healthy(), "is_healthy should be false");
    TEST_ASSERT(!unhealthy.is_degraded(), "is_degraded should be false");
    TEST_ASSERT(unhealthy.is_unhealthy(), "is_unhealthy should be true");
    TEST_PASS("Unhealthy helpers work");

    return true;
}

// =============================================================================
// 테스트 14: 총 체크 카운트
// =============================================================================
bool test_total_checks() {
    std::cout << "\n=== Test 14: Total Checks ===\n";

    HealthCheckerConfig config;
    config.enable_resource_monitoring = false;
    HealthChecker checker(config);

    uint64_t before = checker.total_checks();
    checker.check_all();
    uint64_t after = checker.total_checks();

    TEST_ASSERT(after == before + 1, "Total checks should increase by 1");
    TEST_PASS("Total checks counted");

    return true;
}

// =============================================================================
// 테스트 15: 예외 처리
// =============================================================================
bool test_exception_handling() {
    std::cout << "\n=== Test 15: Exception Handling ===\n";

    HealthCheckerConfig config;
    config.enable_resource_monitoring = false;
    HealthChecker checker(config);

    // 예외 발생 체크 등록
    checker.register_check("exception_check", []() -> ComponentHealth {
        throw std::runtime_error("Test exception");
    });

    // check_all은 예외를 잡아서 Unhealthy로 처리해야 함
    auto health = checker.check_all();

    bool found = false;
    for (const auto& c : health.components) {
        if (c.name == "exception_check") {
            found = true;
            TEST_ASSERT(c.status == HealthStatus::Unhealthy, "Should be unhealthy");
            TEST_ASSERT(c.message.find("exception") != std::string::npos,
                        "Message should contain 'exception'");
        }
    }

    TEST_ASSERT(found, "Exception component should be in results");
    TEST_PASS("Exception handled gracefully");

    return true;
}

// =============================================================================
// 메인
// =============================================================================
int main() {
    std::cout << "========================================\n";
    std::cout << "  Health Check Test (TASK_22)\n";
    std::cout << "========================================\n";

    // 테스트 실행
    std::vector<std::pair<std::string, std::function<bool()>>> tests = {
        {"Status Names", test_status_names},
        {"Basic Registration", test_basic_registration},
        {"Check All", test_check_all},
        {"Check Component", test_check_component},
        {"Resource Monitoring", test_resource_monitoring},
        {"Alert Callbacks", test_alert_callbacks},
        {"Config", test_config},
        {"Overall Status", test_overall_status},
        {"make_simple_check", test_make_simple_check},
        {"make_conditional_check", test_make_conditional_check},
        {"History", test_history},
        {"Health Ratio", test_health_ratio},
        {"ComponentHealth Helpers", test_component_health_helpers},
        {"Total Checks", test_total_checks},
        {"Exception Handling", test_exception_handling},
    };

    int test_count = 0;
    for (const auto& [name, func] : tests) {
        ++test_count;
        try {
            func();
        } catch (const std::exception& e) {
            std::cerr << "  [EXCEPTION] " << name << ": " << e.what() << "\n";
            ++g_test_failed;
        }
    }

    // 결과 출력
    std::cout << "\n========================================\n";
    std::cout << "  Test Results\n";
    std::cout << "========================================\n";
    std::cout << "  Tests run:    " << test_count << "\n";
    std::cout << "  Assertions:   " << (g_test_passed + g_test_failed) << "\n";
    std::cout << "  Passed:       " << g_test_passed << "\n";
    std::cout << "  Failed:       " << g_test_failed << "\n";
    std::cout << "========================================\n";

    if (g_test_failed == 0) {
        std::cout << "\n  ALL TESTS PASSED!\n\n";
        return 0;
    } else {
        std::cout << "\n  SOME TESTS FAILED!\n\n";
        return 1;
    }
}
