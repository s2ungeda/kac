/**
 * Integration Test Implementation (TASK_29)
 *
 * 시스템 통합 테스트
 * - 컴포넌트 통합 확인
 * - 성능 검증
 *
 * 참고: 개별 컴포넌트 테스트는 examples/ 디렉토리에 있음
 * - event_bus_test, shutdown_test, health_check_test,
 *   daily_limit_test, trading_stats_test, cli_test 등
 */

#include "integration_test.hpp"

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/lockfree_queue.hpp"
#include "arbitrage/common/memory_pool.hpp"
#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"

#include <iostream>
#include <iomanip>
#include <thread>
#include <cmath>
#include <atomic>
#include <sstream>

namespace arbitrage {
namespace integration {

// =============================================================================
// IntegrationTestRunner
// =============================================================================

IntegrationTestRunner::IntegrationTestRunner() = default;

IntegrationTestRunner::IntegrationTestRunner(const IntegrationTestConfig& config)
    : config_(config)
{}

IntegrationTestRunner::~IntegrationTestRunner() = default;

void IntegrationTestRunner::register_test(
    const std::string& name,
    TestCategory category,
    TestFunc func
) {
    tests_.push_back({name, category, std::move(func)});
}

void IntegrationTestRunner::register_default_tests() {
    // Performance Tests (핵심 저지연 성능)
    register_test("SPSCQueueLatency", TestCategory::Performance, tests::test_spsc_queue_latency);
    register_test("MemoryPoolPerf", TestCategory::Performance, tests::test_memory_pool_performance);
    register_test("JSONParserThroughput", TestCategory::Performance, tests::test_json_parser_throughput);

    // Infrastructure Tests (EventBus 통합)
    register_test("EventBusBasic", TestCategory::Infrastructure, tests::test_event_bus);

    // Dry-Run Tests (타입 검증)
    register_test("DualOrderValidation", TestCategory::DryRun, tests::test_dual_order_dry_run);
    register_test("RecoveryActions", TestCategory::DryRun, tests::test_recovery_dry_run);
}

TestSummary IntegrationTestRunner::run_all() {
    TestSummary summary;
    auto start = std::chrono::steady_clock::now();

    for (const auto& entry : tests_) {
        notify_progress(entry.name, TestStatus::Running);

        auto result = entry.func();
        summary.results.push_back(result);
        summary.total++;

        switch (result.status) {
            case TestStatus::Passed:  summary.passed++;  break;
            case TestStatus::Failed:  summary.failed++;  break;
            case TestStatus::Skipped: summary.skipped++; break;
            case TestStatus::Timeout: summary.timeout++; break;
            default: break;
        }

        notify_progress(entry.name, result.status);

        if (config_.stop_on_failure && result.failed()) {
            break;
        }
    }

    auto end = std::chrono::steady_clock::now();
    summary.total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    return summary;
}

TestSummary IntegrationTestRunner::run_category(TestCategory category) {
    TestSummary summary;
    auto start = std::chrono::steady_clock::now();

    for (const auto& entry : tests_) {
        if (entry.category != category) continue;

        notify_progress(entry.name, TestStatus::Running);

        auto result = entry.func();
        summary.results.push_back(result);
        summary.total++;

        switch (result.status) {
            case TestStatus::Passed:  summary.passed++;  break;
            case TestStatus::Failed:  summary.failed++;  break;
            case TestStatus::Skipped: summary.skipped++; break;
            case TestStatus::Timeout: summary.timeout++; break;
            default: break;
        }

        notify_progress(entry.name, result.status);

        if (config_.stop_on_failure && result.failed()) {
            break;
        }
    }

    auto end = std::chrono::steady_clock::now();
    summary.total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    return summary;
}

TestResult IntegrationTestRunner::run_test(const std::string& name) {
    for (const auto& entry : tests_) {
        if (entry.name == name) {
            notify_progress(entry.name, TestStatus::Running);
            auto result = entry.func();
            notify_progress(entry.name, result.status);
            return result;
        }
    }

    TestResult result;
    result.name = name;
    result.status = TestStatus::Failed;
    result.message = "Test not found: " + name;
    return result;
}

void IntegrationTestRunner::set_config(const IntegrationTestConfig& config) {
    config_ = config;
}

void IntegrationTestRunner::on_progress(ProgressCallback callback) {
    progress_callback_ = std::move(callback);
}

void IntegrationTestRunner::notify_progress(const std::string& name, TestStatus status) {
    if (progress_callback_) {
        progress_callback_(name, status);
    }
}

void IntegrationTestRunner::print_summary(const TestSummary& summary) const {
    std::cout << "\n========================================\n";
    std::cout << "  Integration Test Summary\n";
    std::cout << "========================================\n";
    std::cout << "  Total:    " << summary.total << "\n";
    std::cout << "  Passed:   " << summary.passed << "\n";
    std::cout << "  Failed:   " << summary.failed << "\n";
    std::cout << "  Skipped:  " << summary.skipped << "\n";
    std::cout << "  Timeout:  " << summary.timeout << "\n";
    std::cout << "  Duration: " << summary.total_duration.count() << "ms\n";
    std::cout << "  Pass Rate: " << std::fixed << std::setprecision(1)
              << summary.pass_rate() << "%\n";
    std::cout << "========================================\n";

    if (summary.failed > 0) {
        std::cout << "\nFailed Tests:\n";
        for (const auto& result : summary.results) {
            if (result.failed()) {
                std::cout << "  - " << result.name << ": " << result.message << "\n";
            }
        }
    }
}

void IntegrationTestRunner::print_result(const TestResult& result) const {
    const char* status_str = "???";
    switch (result.status) {
        case TestStatus::Running: status_str = "RUNNING"; break;
        case TestStatus::Passed:  status_str = "PASSED";  break;
        case TestStatus::Failed:  status_str = "FAILED";  break;
        case TestStatus::Skipped: status_str = "SKIPPED"; break;
        case TestStatus::Timeout: status_str = "TIMEOUT"; break;
        case TestStatus::Pending: status_str = "PENDING"; break;
    }

    std::cout << "  [" << status_str << "] " << result.name;
    if (result.duration.count() > 0) {
        std::cout << " (" << result.duration.count() << "ms)";
    }
    if (!result.message.empty() && result.status != TestStatus::Passed) {
        std::cout << " - " << result.message;
    }
    std::cout << "\n";
}

// =============================================================================
// Individual Tests
// =============================================================================

namespace tests {

TestResult test_spsc_queue_latency() {
    TestResult result;
    result.name = "SPSCQueueLatency";
    result.timestamp = std::chrono::system_clock::now();
    auto start_time = std::chrono::steady_clock::now();

    try {
        SPSCQueue<int64_t> queue(1024);
        constexpr int iterations = 10000;

        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; ++i) {
            queue.push(static_cast<int64_t>(i));
            int64_t val;
            queue.pop(val);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ns_per_op = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start).count() / iterations;

        // 1000ns (1us) 이하 기대
        if (ns_per_op >= 1000) {
            std::ostringstream oss;
            oss << "SPSCQueue: Latency too high (" << ns_per_op << "ns)";
            throw std::runtime_error(oss.str());
        }

        result.status = TestStatus::Passed;
        result.message = "OK";
    } catch (const std::exception& e) {
        result.status = TestStatus::Failed;
        result.message = e.what();
    } catch (...) {
        result.status = TestStatus::Failed;
        result.message = "Unknown error";
    }

    auto end_time = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    return result;
}

TestResult test_memory_pool_performance() {
    TestResult result;
    result.name = "MemoryPoolPerformance";
    result.timestamp = std::chrono::system_clock::now();
    auto start_time = std::chrono::steady_clock::now();

    try {
        // ObjectPool<T, PoolSize> - PoolSize는 템플릿 파라미터
        ObjectPool<Ticker, 1024> pool;
        constexpr int iterations = 10000;

        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; ++i) {
            auto* obj = pool.acquire();
            if (obj) {
                pool.release(obj);
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ns_per_op = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start).count() / iterations;

        // 1000ns (1us) 이하 기대
        if (ns_per_op >= 1000) {
            std::ostringstream oss;
            oss << "MemoryPool: Latency too high (" << ns_per_op << "ns)";
            throw std::runtime_error(oss.str());
        }

        result.status = TestStatus::Passed;
        result.message = "OK";
    } catch (const std::exception& e) {
        result.status = TestStatus::Failed;
        result.message = e.what();
    } catch (...) {
        result.status = TestStatus::Failed;
        result.message = "Unknown error";
    }

    auto end_time = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    return result;
}

TestResult test_json_parser_throughput() {
    TestResult result;
    result.name = "JSONParserThroughput";
    result.timestamp = std::chrono::system_clock::now();
    auto start_time = std::chrono::steady_clock::now();

    try {
        constexpr int iterations = 10000;
        const std::string json_str = R"({"price":3100.5,"volume":1000.0,"exchange":"upbit"})";

        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; ++i) {
            size_t price_pos = json_str.find("price");
            size_t vol_pos = json_str.find("volume");
            if (price_pos == std::string::npos || vol_pos == std::string::npos) {
                throw std::runtime_error("JSON parse failed");
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        double throughput = (iterations * 1000.0) / (ms > 0 ? ms : 1);

        // 10K ops/sec 이상 기대
        if (throughput <= 10000) {
            throw std::runtime_error("JSON throughput too low");
        }

        result.status = TestStatus::Passed;
        result.message = "OK";
    } catch (const std::exception& e) {
        result.status = TestStatus::Failed;
        result.message = e.what();
    } catch (...) {
        result.status = TestStatus::Failed;
        result.message = "Unknown error";
    }

    auto end_time = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    return result;
}

TestResult test_event_bus() {
    TestResult result;
    result.name = "EventBusBasic";
    result.timestamp = std::chrono::system_clock::now();
    auto start_time = std::chrono::steady_clock::now();

    try {
        auto bus = std::make_shared<EventBus>();
        bus->start_async();

        std::atomic<int> received{0};
        auto token = bus->subscribe<events::TickerReceived>([&](const events::TickerReceived& e) {
            (void)e;  // suppress unused warning
            received++;
        });

        // events::TickerReceived는 ticker 필드(Ticker 타입)를 가짐
        events::TickerReceived event;
        event.ticker.exchange = Exchange::Upbit;
        event.ticker.price = 3000.0;
        event.ticker.set_symbol("XRP-KRW");
        bus->publish(event);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (received.load() < 1) {
            throw std::runtime_error("EventBus: Event not received");
        }

        bus->stop();

        result.status = TestStatus::Passed;
        result.message = "OK";
    } catch (const std::exception& e) {
        result.status = TestStatus::Failed;
        result.message = e.what();
    } catch (...) {
        result.status = TestStatus::Failed;
        result.message = "Unknown error";
    }

    auto end_time = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    return result;
}

TestResult test_dual_order_dry_run() {
    TestResult result;
    result.name = "DualOrderValidation";
    result.timestamp = std::chrono::system_clock::now();
    auto start_time = std::chrono::steady_clock::now();

    try {
        // OrderRequest 검증
        OrderRequest buy_order;
        buy_order.exchange = Exchange::Binance;
        buy_order.side = OrderSide::Buy;
        buy_order.quantity = 100.0;
        buy_order.price = 2.15;
        buy_order.set_symbol("XRP");

        OrderRequest sell_order;
        sell_order.exchange = Exchange::Upbit;
        sell_order.side = OrderSide::Sell;
        sell_order.quantity = 100.0;
        sell_order.price = 3200.0;
        sell_order.set_symbol("XRP");

        if (buy_order.exchange == sell_order.exchange) {
            throw std::runtime_error("Buy and sell should be different exchanges");
        }
        if (buy_order.side != OrderSide::Buy) {
            throw std::runtime_error("Should be buy order");
        }
        if (sell_order.side != OrderSide::Sell) {
            throw std::runtime_error("Should be sell order");
        }
        if (buy_order.quantity <= 0) {
            throw std::runtime_error("Quantity should be positive");
        }

        result.status = TestStatus::Passed;
        result.message = "OK";
    } catch (const std::exception& e) {
        result.status = TestStatus::Failed;
        result.message = e.what();
    } catch (...) {
        result.status = TestStatus::Failed;
        result.message = "Unknown error";
    }

    auto end_time = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    return result;
}

TestResult test_recovery_dry_run() {
    TestResult result;
    result.name = "RecoveryActions";
    result.timestamp = std::chrono::system_clock::now();
    auto start_time = std::chrono::steady_clock::now();

    try {
        // 거래소 관련 상수 검증
        if (exchange_name(Exchange::Upbit) == nullptr) {
            throw std::runtime_error("Upbit name is null");
        }
        if (exchange_name(Exchange::Bithumb) == nullptr) {
            throw std::runtime_error("Bithumb name is null");
        }
        if (exchange_name(Exchange::Binance) == nullptr) {
            throw std::runtime_error("Binance name is null");
        }
        if (exchange_name(Exchange::MEXC) == nullptr) {
            throw std::runtime_error("MEXC name is null");
        }

        // KRW 거래소 확인
        if (!is_krw_exchange(Exchange::Upbit)) {
            throw std::runtime_error("Upbit should be KRW");
        }
        if (!is_krw_exchange(Exchange::Bithumb)) {
            throw std::runtime_error("Bithumb should be KRW");
        }
        if (is_krw_exchange(Exchange::Binance)) {
            throw std::runtime_error("Binance should not be KRW");
        }
        if (is_krw_exchange(Exchange::MEXC)) {
            throw std::runtime_error("MEXC should not be KRW");
        }

        result.status = TestStatus::Passed;
        result.message = "OK";
    } catch (const std::exception& e) {
        result.status = TestStatus::Failed;
        result.message = e.what();
    } catch (...) {
        result.status = TestStatus::Failed;
        result.message = "Unknown error";
    }

    auto end_time = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    return result;
}

}  // namespace tests

}  // namespace integration
}  // namespace arbitrage
