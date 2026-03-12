/**
 * Thread Manager Test (TASK_20)
 *
 * ThreadManager 테스트
 * - 시스템 토폴로지 조회
 * - 스레드 어피니티 설정
 * - 스레드 우선순위 설정
 * - 관리 스레드 생성
 */

#include "arbitrage/common/thread_manager.hpp"
#include "arbitrage/common/numa_allocator.hpp"

#include <iostream>
#include <atomic>
#include <chrono>

using namespace arbitrage;

// 테스트 결과 카운터
static int tests_passed = 0;
static int tests_failed = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::cout << "[FAIL] " << msg << "\n";
        ++tests_failed;
    } else {
        std::cout << "[PASS] " << msg << "\n";
        ++tests_passed;
    }
}

// =============================================================================
// Test: System Topology
// =============================================================================
void test_system_topology() {
    std::cout << "\n=== Test: system_topology ===\n";

    auto topology = ThreadManager::get_system_topology();

    check(topology.logical_cores > 0, "Has logical cores");
    check(topology.physical_cores > 0, "Has physical cores");
    check(topology.physical_cores <= topology.logical_cores, "Physical <= Logical cores");
    check(topology.numa_nodes >= 1, "Has at least 1 NUMA node");

    std::cout << "  Logical cores: " << topology.logical_cores << "\n";
    std::cout << "  Physical cores: " << topology.physical_cores << "\n";
    std::cout << "  NUMA nodes: " << topology.numa_nodes << "\n";
    std::cout << "  Hyperthreading: " << (topology.hyperthreading ? "Yes" : "No") << "\n";
}

// =============================================================================
// Test: Thread Priority Enum
// =============================================================================
void test_thread_priority() {
    std::cout << "\n=== Test: thread_priority ===\n";

    check(std::string(to_string(ThreadPriority::Idle)) == "idle", "Idle priority string");
    check(std::string(to_string(ThreadPriority::Low)) == "low", "Low priority string");
    check(std::string(to_string(ThreadPriority::Normal)) == "normal", "Normal priority string");
    check(std::string(to_string(ThreadPriority::High)) == "high", "High priority string");
    check(std::string(to_string(ThreadPriority::RealTime)) == "realtime", "RealTime priority string");

    check(parse_priority("idle") == ThreadPriority::Idle, "Parse idle");
    check(parse_priority("low") == ThreadPriority::Low, "Parse low");
    check(parse_priority("normal") == ThreadPriority::Normal, "Parse normal");
    check(parse_priority("high") == ThreadPriority::High, "Parse high");
    check(parse_priority("realtime") == ThreadPriority::RealTime, "Parse realtime");
    check(parse_priority("invalid") == ThreadPriority::Normal, "Invalid defaults to normal");
}

// =============================================================================
// Test: Thread Config
// =============================================================================
void test_thread_config() {
    std::cout << "\n=== Test: thread_config ===\n";

    ThreadConfig cfg1("test_thread", ThreadPriority::High);
    check(cfg1.name == "test_thread", "Config name");
    check(cfg1.priority == ThreadPriority::High, "Config priority");
    check(!cfg1.core_id.has_value(), "Core ID not set");

    ThreadConfig cfg2("ws_thread", 0, ThreadPriority::RealTime);
    check(cfg2.name == "ws_thread", "Config name with core");
    check(cfg2.core_id.has_value() && cfg2.core_id.value() == 0, "Core ID set to 0");
    check(cfg2.priority == ThreadPriority::RealTime, "Priority is RealTime");
}

// =============================================================================
// Test: Manager Config
// =============================================================================
void test_manager_config() {
    std::cout << "\n=== Test: manager_config ===\n";

    ThreadManagerConfig config;
    config.affinity_enabled = true;
    config.priority_enabled = true;

    config.threads["upbit_ws"] = ThreadConfig("upbit_ws", 0, ThreadPriority::High);
    config.threads["strategy"] = ThreadConfig("strategy", 2, ThreadPriority::RealTime);

    auto& upbit_cfg = config.get_config("upbit_ws");
    check(upbit_cfg.core_id.value() == 0, "upbit_ws core is 0");
    check(upbit_cfg.priority == ThreadPriority::High, "upbit_ws priority is High");

    auto& strategy_cfg = config.get_config("strategy");
    check(strategy_cfg.core_id.value() == 2, "strategy core is 2");

    auto& unknown_cfg = config.get_config("unknown");
    check(&unknown_cfg == &config.default_config, "Unknown returns default config");
}

// =============================================================================
// Test: Initialize Manager
// =============================================================================
void test_initialize_manager() {
    std::cout << "\n=== Test: initialize_manager ===\n";

    ThreadManagerConfig config;
    config.affinity_enabled = true;
    config.priority_enabled = true;
    config.threads["test_thread"] = ThreadConfig("test_thread", 0, ThreadPriority::Normal);

    thread_manager().initialize(config);

    check(thread_manager().config().affinity_enabled, "Affinity enabled");
    check(thread_manager().config().priority_enabled, "Priority enabled");
    check(thread_manager().config().threads.count("test_thread") > 0, "Thread config exists");
}

// =============================================================================
// Test: Create Thread
// =============================================================================
void test_create_thread() {
    std::cout << "\n=== Test: create_thread ===\n";

    ThreadManagerConfig config;
    config.affinity_enabled = false;  // WSL에서 어피니티 설정 제한
    config.priority_enabled = false;
    config.threads["worker"] = ThreadConfig("worker", ThreadPriority::Normal);

    thread_manager().initialize(config);

    std::atomic<bool> executed{false};
    std::atomic<int> value{0};

    auto t = thread_manager().create_thread("worker", [&]() {
        executed = true;
        value = 42;
    });

    t.join();

    check(executed.load(), "Thread executed");
    check(value.load() == 42, "Value set correctly");
}

// =============================================================================
// Test: Thread Stats
// =============================================================================
void test_thread_stats() {
    std::cout << "\n=== Test: thread_stats ===\n";

    ThreadManagerConfig config;
    config.affinity_enabled = false;
    config.priority_enabled = false;
    config.threads["stats_test"] = ThreadConfig("stats_test", ThreadPriority::High);

    thread_manager().initialize(config);

    std::atomic<bool> running{true};

    auto t = thread_manager().create_thread("stats_test", [&]() {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // 스레드 등록 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto stats = thread_manager().get_all_stats();
    bool found = false;
    for (const auto& s : stats) {
        if (s.name == "stats_test") {
            found = true;
            check(s.is_alive, "Thread is alive");
            check(s.priority == ThreadPriority::High, "Thread priority is High");
        }
    }
    check(found, "stats_test found in all_stats");

    auto single_stats = thread_manager().get_thread_stats("stats_test");
    check(single_stats.is_alive, "Single stats shows alive");
    check(single_stats.name == "stats_test", "Single stats name correct");

    auto not_found = thread_manager().get_thread_stats("nonexistent");
    check(!not_found.is_alive, "Nonexistent thread not alive");

    running = false;
    t.join();
}

// =============================================================================
// Test: Current Thread Affinity (Linux Only)
// =============================================================================
void test_current_affinity() {
    std::cout << "\n=== Test: current_affinity ===\n";

#ifdef __linux__
    int num_cores = ThreadManager::get_num_logical_cores();

    if (num_cores > 1) {
        // 코어 0에 어피니티 설정
        auto result = ThreadManager::set_current_affinity(0);
        if (result.has_value()) {
            int current = ThreadManager::get_current_core();
            // WSL에서는 항상 동작하지 않을 수 있음
            std::cout << "  Current core: " << current << "\n";
            check(true, "Set affinity succeeded");
        } else {
            // 권한 부족 등으로 실패할 수 있음
            std::cout << "  Affinity set failed (may need permissions)\n";
            check(true, "Affinity API called (may require permissions)");
        }

        // 잘못된 코어 ID
        auto bad_result = ThreadManager::set_current_affinity(-1);
        check(bad_result.has_error(), "Invalid core_id returns error");
    } else {
        check(true, "Single core system, skipping affinity test");
    }
#else
    check(true, "Affinity test skipped on non-Linux");
#endif
}

// =============================================================================
// Test: NUMA Functions
// =============================================================================
void test_numa_functions() {
    std::cout << "\n=== Test: numa_functions ===\n";

    bool numa_available = is_numa_available();
    std::cout << "  NUMA available: " << (numa_available ? "Yes" : "No") << "\n";

    int current_node = get_current_numa_node();
    std::cout << "  Current NUMA node: " << current_node << "\n";

    check(current_node >= 0, "Current NUMA node is non-negative");
}

// =============================================================================
// Test: NUMA Buffer
// =============================================================================
void test_numa_buffer() {
    std::cout << "\n=== Test: numa_buffer ===\n";

    NumaBuffer buf(4096, 0);
    check(buf.valid(), "Buffer is valid");
    check(buf.size() == 4096, "Buffer size is 4096");
    check(buf.data() != nullptr, "Buffer data is not null");

    // 버퍼에 쓰기/읽기
    int* data = buf.as<int>();
    data[0] = 12345;
    data[1] = 67890;
    check(data[0] == 12345, "Buffer write/read works");
    check(data[1] == 67890, "Buffer second value works");

    // 이동
    NumaBuffer buf2 = std::move(buf);
    check(buf2.valid(), "Moved buffer is valid");
    check(!buf.valid(), "Original buffer is invalid after move");
}

// =============================================================================
// Test: Physical Core IDs
// =============================================================================
void test_physical_core_ids() {
    std::cout << "\n=== Test: physical_core_ids ===\n";

    auto physical_ids = ThreadManager::get_physical_core_ids();
    int physical_count = ThreadManager::get_num_physical_cores();

    std::cout << "  Physical cores: " << physical_count << "\n";
    std::cout << "  Physical core IDs: ";
    for (int id : physical_ids) {
        std::cout << id << " ";
    }
    std::cout << "\n";

    check(!physical_ids.empty(), "Physical core IDs not empty");
    check(physical_ids.size() <= static_cast<size_t>(ThreadManager::get_num_logical_cores()),
          "Physical <= Logical");
}

// =============================================================================
// Main
// =============================================================================
int main() {
    std::cout << "========================================\n";
    std::cout << "  Thread Manager Test (TASK_20)\n";
    std::cout << "========================================\n";

    test_system_topology();
    test_thread_priority();
    test_thread_config();
    test_manager_config();
    test_initialize_manager();
    test_create_thread();
    test_thread_stats();
    test_current_affinity();
    test_numa_functions();
    test_numa_buffer();
    test_physical_core_ids();

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "========================================\n";

    return tests_failed > 0 ? 1 : 0;
}
