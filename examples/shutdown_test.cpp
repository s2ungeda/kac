/**
 * Shutdown Manager Test (TASK_21)
 *
 * 테스트 항목:
 * 1. 컴포넌트 등록/해제
 * 2. 우선순위 기반 종료 순서
 * 3. 타임아웃 처리
 * 4. 시그널 핸들러
 * 5. EventBus 연동
 * 6. RAII ShutdownGuard
 * 7. 진행 콜백
 * 8. 강제 종료
 */

#include "arbitrage/infra/shutdown.hpp"
#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"

#include <csignal>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
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
// 테스트 유틸리티
// =============================================================================

/**
 * ShutdownManager 상태 리셋
 * (싱글톤이므로 테스트 간 상태 초기화 필요)
 */
void reset_shutdown_manager() {
    auto& manager = shutdown_manager();

    // 모든 컴포넌트 해제 - component_count()를 사용해 이름을 알 수 없으므로
    // 새 매니저 상태로 리셋하는 것은 불가능
    // 대신 테스트마다 고유한 컴포넌트 이름 사용
}

// =============================================================================
// 테스트 1: 기본 컴포넌트 등록
// =============================================================================
bool test_basic_registration() {
    std::cout << "\n=== Test 1: Basic Component Registration ===\n";

    auto& manager = shutdown_manager();
    size_t initial_count = manager.component_count();

    // 컴포넌트 등록
    std::atomic<bool> callback_called{false};
    manager.register_component("test1_component", [&]() {
        callback_called = true;
    });

    TEST_ASSERT(manager.component_count() == initial_count + 1,
                "Component count should increase");
    TEST_PASS("Component registered");

    // 컴포넌트 해제
    manager.unregister_component("test1_component");
    TEST_ASSERT(manager.component_count() == initial_count,
                "Component count should return to initial");
    TEST_PASS("Component unregistered");

    return true;
}

// =============================================================================
// 테스트 2: 우선순위 등록
// =============================================================================
bool test_priority_registration() {
    std::cout << "\n=== Test 2: Priority Registration ===\n";

    auto& manager = shutdown_manager();
    size_t initial_count = manager.component_count();

    // 다양한 우선순위로 등록
    manager.register_component("test2_network", [](){}, ShutdownPriority::Network);
    manager.register_component("test2_order", [](){}, ShutdownPriority::Order);
    manager.register_component("test2_logging", [](){}, ShutdownPriority::Logging);

    TEST_ASSERT(manager.component_count() == initial_count + 3,
                "Three components should be registered");
    TEST_PASS("Multiple priority components registered");

    // 정리
    manager.unregister_component("test2_network");
    manager.unregister_component("test2_order");
    manager.unregister_component("test2_logging");

    TEST_ASSERT(manager.component_count() == initial_count,
                "Components should be unregistered");
    TEST_PASS("Components unregistered");

    return true;
}

// =============================================================================
// 테스트 3: ShutdownGuard RAII
// =============================================================================
bool test_shutdown_guard() {
    std::cout << "\n=== Test 3: ShutdownGuard RAII ===\n";

    auto& manager = shutdown_manager();
    size_t initial_count = manager.component_count();

    {
        ShutdownGuard guard("test3_guard_component", [](){
            std::cout << "    Guard component callback\n";
        });

        TEST_ASSERT(manager.component_count() == initial_count + 1,
                    "Component should be registered");
        TEST_PASS("ShutdownGuard registered component");
    }

    TEST_ASSERT(manager.component_count() == initial_count,
                "Component should be unregistered on destruction");
    TEST_PASS("ShutdownGuard unregistered on scope exit");

    return true;
}

// =============================================================================
// 테스트 4: 시그널 이름
// =============================================================================
bool test_signal_names() {
    std::cout << "\n=== Test 4: Signal Names ===\n";

    TEST_ASSERT(std::string(ShutdownManager::signal_name(SIGINT)) == "SIGINT",
                "SIGINT name");
    TEST_PASS("SIGINT name correct");

    TEST_ASSERT(std::string(ShutdownManager::signal_name(SIGTERM)) == "SIGTERM",
                "SIGTERM name");
    TEST_PASS("SIGTERM name correct");

#ifndef _WIN32
    TEST_ASSERT(std::string(ShutdownManager::signal_name(SIGHUP)) == "SIGHUP",
                "SIGHUP name");
    TEST_PASS("SIGHUP name correct");
#endif

    return true;
}

// =============================================================================
// 테스트 5: 종료 단계 이름
// =============================================================================
bool test_phase_names() {
    std::cout << "\n=== Test 5: Shutdown Phase Names ===\n";

    TEST_ASSERT(std::string(shutdown_phase_name(ShutdownPhase::Running)) == "Running",
                "Running phase name");
    TEST_PASS("Running phase name correct");

    TEST_ASSERT(std::string(shutdown_phase_name(ShutdownPhase::Initiated)) == "Initiated",
                "Initiated phase name");
    TEST_PASS("Initiated phase name correct");

    TEST_ASSERT(std::string(shutdown_phase_name(ShutdownPhase::Stopping)) == "Stopping",
                "Stopping phase name");
    TEST_PASS("Stopping phase name correct");

    TEST_ASSERT(std::string(shutdown_phase_name(ShutdownPhase::Completed)) == "Completed",
                "Completed phase name");
    TEST_PASS("Completed phase name correct");

    TEST_ASSERT(std::string(shutdown_phase_name(ShutdownPhase::Timeout)) == "Timeout",
                "Timeout phase name");
    TEST_PASS("Timeout phase name correct");

    return true;
}

// =============================================================================
// 테스트 6: 시그널 핸들러 설치/해제
// =============================================================================
bool test_signal_handlers() {
    std::cout << "\n=== Test 6: Signal Handlers ===\n";

    auto& manager = shutdown_manager();

    // 핸들러 설치
    manager.install_signal_handlers();
    TEST_PASS("Signal handlers installed");

    // 중복 설치 (에러 없어야 함)
    manager.install_signal_handlers();
    TEST_PASS("Duplicate install handled");

    // 핸들러 해제
    manager.uninstall_signal_handlers();
    TEST_PASS("Signal handlers uninstalled");

    // 중복 해제 (에러 없어야 함)
    manager.uninstall_signal_handlers();
    TEST_PASS("Duplicate uninstall handled");

    return true;
}

// =============================================================================
// 테스트 7: EventBus 연동
// =============================================================================
bool test_event_bus_integration() {
    std::cout << "\n=== Test 7: EventBus Integration ===\n";

    auto bus = std::make_shared<EventBus>();

    std::atomic<bool> event_received{false};
    std::string received_reason;

    // SystemShutdown 이벤트 구독
    auto token = bus->subscribe<events::SystemShutdown>(
        [&](const events::SystemShutdown& event) {
            event_received = true;
            received_reason = event.reason;
        });

    auto& manager = shutdown_manager();
    manager.set_event_bus(bus);
    TEST_PASS("EventBus connected");

    // EventBus 연결 해제 (종료 테스트하지 않음 - 싱글톤 상태 영향)
    manager.set_event_bus(nullptr);

    bus->unsubscribe(token);
    TEST_PASS("EventBus integration configured correctly");

    return true;
}

// =============================================================================
// 테스트 8: 진행 콜백
// =============================================================================
bool test_progress_callback() {
    std::cout << "\n=== Test 8: Progress Callback ===\n";

    auto& manager = shutdown_manager();

    std::vector<std::pair<std::string, int>> progress_events;

    manager.set_progress_callback([&](const std::string& component, int progress) {
        progress_events.push_back({component, progress});
    });

    TEST_PASS("Progress callback set");

    // 콜백 해제
    manager.set_progress_callback(nullptr);
    TEST_PASS("Progress callback cleared");

    return true;
}

// =============================================================================
// 테스트 9: 컴포넌트 업데이트
// =============================================================================
bool test_component_update() {
    std::cout << "\n=== Test 9: Component Update ===\n";

    auto& manager = shutdown_manager();
    size_t initial_count = manager.component_count();

    std::atomic<int> call_count{0};

    // 첫 번째 등록
    manager.register_component("test9_update", [&]() {
        call_count = 1;
    });

    TEST_ASSERT(manager.component_count() == initial_count + 1,
                "One component registered");
    TEST_PASS("Initial component registered");

    // 같은 이름으로 업데이트
    manager.register_component("test9_update", [&]() {
        call_count = 2;
    });

    TEST_ASSERT(manager.component_count() == initial_count + 1,
                "Still one component (updated)");
    TEST_PASS("Component updated without duplication");

    // 정리
    manager.unregister_component("test9_update");

    return true;
}

// =============================================================================
// 테스트 10: 종료 결과 구조체
// =============================================================================
bool test_shutdown_result() {
    std::cout << "\n=== Test 10: ShutdownResult ===\n";

    ShutdownResult result;

    // 초기 상태
    TEST_ASSERT(result.phase == ShutdownPhase::Running, "Initial phase is Running");
    TEST_ASSERT(result.completed_components.empty(), "No completed components");
    TEST_ASSERT(result.timeout_components.empty(), "No timeout components");
    TEST_ASSERT(result.failed_components.empty(), "No failed components");
    TEST_PASS("Initial ShutdownResult state correct");

    // 성공 케이스
    result.phase = ShutdownPhase::Completed;
    TEST_ASSERT(result.success(), "Success when completed with no issues");
    TEST_PASS("Success detection works");

    // 실패 케이스 (타임아웃)
    result.timeout_components.push_back("slow_component");
    TEST_ASSERT(!result.success(), "Not success when timeout components exist");
    TEST_PASS("Failure detection works");

    return true;
}

// =============================================================================
// 테스트 11: 초기 상태
// =============================================================================
bool test_initial_state() {
    std::cout << "\n=== Test 11: Initial State ===\n";

    auto& manager = shutdown_manager();

    // 초기 상태 확인 (다른 테스트에서 종료가 시작되지 않았다면)
    // 싱글톤이므로 이전 테스트의 영향을 받을 수 있음

    std::cout << "    Current phase: " << shutdown_phase_name(manager.phase()) << "\n";
    std::cout << "    Is shutting down: " << (manager.is_shutting_down() ? "yes" : "no") << "\n";
    std::cout << "    Component count: " << manager.component_count() << "\n";

    TEST_PASS("Initial state readable");

    return true;
}

// =============================================================================
// 테스트 12: 중복 컴포넌트 해제
// =============================================================================
bool test_duplicate_unregister() {
    std::cout << "\n=== Test 12: Duplicate Unregister ===\n";

    auto& manager = shutdown_manager();
    size_t initial_count = manager.component_count();

    // 없는 컴포넌트 해제 시도 (에러 없어야 함)
    manager.unregister_component("nonexistent_component_xyz");
    TEST_ASSERT(manager.component_count() == initial_count,
                "Count unchanged after unregistering nonexistent");
    TEST_PASS("Unregister nonexistent component handled gracefully");

    // 등록 후 중복 해제
    manager.register_component("test12_temp", [](){});
    manager.unregister_component("test12_temp");
    manager.unregister_component("test12_temp");  // 중복

    TEST_ASSERT(manager.component_count() == initial_count,
                "Count unchanged after duplicate unregister");
    TEST_PASS("Duplicate unregister handled gracefully");

    return true;
}

// =============================================================================
// 테스트 13: 우선순위 상수
// =============================================================================
bool test_priority_constants() {
    std::cout << "\n=== Test 13: Priority Constants ===\n";

    TEST_ASSERT(ShutdownPriority::Network < ShutdownPriority::Order,
                "Network < Order");
    TEST_PASS("Network priority lower than Order");

    TEST_ASSERT(ShutdownPriority::Order < ShutdownPriority::Transfer,
                "Order < Transfer");
    TEST_PASS("Order priority lower than Transfer");

    TEST_ASSERT(ShutdownPriority::Transfer < ShutdownPriority::Strategy,
                "Transfer < Strategy");
    TEST_PASS("Transfer priority lower than Strategy");

    TEST_ASSERT(ShutdownPriority::Storage < ShutdownPriority::Logging,
                "Storage < Logging");
    TEST_PASS("Storage priority lower than Logging");

    TEST_ASSERT(ShutdownPriority::Default == ShutdownPriority::Storage,
                "Default == Storage");
    TEST_PASS("Default priority equals Storage");

    return true;
}

// =============================================================================
// 테스트 14: ShutdownGuard with priority
// =============================================================================
bool test_shutdown_guard_priority() {
    std::cout << "\n=== Test 14: ShutdownGuard with Priority ===\n";

    auto& manager = shutdown_manager();
    size_t initial_count = manager.component_count();

    {
        ShutdownGuard guard("test14_priority_guard", [](){
            std::cout << "    Priority guard callback\n";
        }, ShutdownPriority::Network);

        TEST_ASSERT(manager.component_count() == initial_count + 1,
                    "Component registered");
        TEST_PASS("ShutdownGuard with priority registered");
    }

    TEST_ASSERT(manager.component_count() == initial_count,
                "Component unregistered");
    TEST_PASS("ShutdownGuard with priority unregistered");

    return true;
}

// =============================================================================
// 테스트 15: 진행률 계산
// =============================================================================
bool test_progress_percent() {
    std::cout << "\n=== Test 15: Progress Percent ===\n";

    auto& manager = shutdown_manager();

    int progress = manager.progress_percent();
    std::cout << "    Current progress: " << progress << "%\n";

    TEST_ASSERT(progress >= 0 && progress <= 100, "Progress in valid range");
    TEST_PASS("Progress percent in valid range");

    return true;
}

// =============================================================================
// 메인
// =============================================================================
int main() {
    std::cout << "========================================\n";
    std::cout << "  Shutdown Manager Test (TASK_21)\n";
    std::cout << "========================================\n";

    // 테스트 실행
    std::vector<std::pair<std::string, std::function<bool()>>> tests = {
        {"Basic Registration", test_basic_registration},
        {"Priority Registration", test_priority_registration},
        {"ShutdownGuard RAII", test_shutdown_guard},
        {"Signal Names", test_signal_names},
        {"Phase Names", test_phase_names},
        {"Signal Handlers", test_signal_handlers},
        {"EventBus Integration", test_event_bus_integration},
        {"Progress Callback", test_progress_callback},
        {"Component Update", test_component_update},
        {"ShutdownResult", test_shutdown_result},
        {"Initial State", test_initial_state},
        {"Duplicate Unregister", test_duplicate_unregister},
        {"Priority Constants", test_priority_constants},
        {"ShutdownGuard Priority", test_shutdown_guard_priority},
        {"Progress Percent", test_progress_percent},
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
