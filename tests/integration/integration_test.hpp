#pragma once

/**
 * Integration Test (TASK_29)
 *
 * 시스템 통합 테스트
 * - 실제 API 연결 (읽기 전용)
 * - 컴포넌트 간 연동
 * - Dry-run 모드 거래
 *
 * ⚠️ Mock/Stub 사용 금지 - 실제 API만 사용
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/error.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace arbitrage {
namespace integration {

// =============================================================================
// 테스트 결과
// =============================================================================

enum class TestStatus {
    Pending,
    Running,
    Passed,
    Failed,
    Skipped,
    Timeout
};

struct TestResult {
    std::string name;
    TestStatus status{TestStatus::Pending};
    std::string message;
    std::chrono::milliseconds duration{0};
    std::chrono::system_clock::time_point timestamp;

    TestResult() : timestamp(std::chrono::system_clock::now()) {}

    bool passed() const { return status == TestStatus::Passed; }
    bool failed() const { return status == TestStatus::Failed; }
};

// =============================================================================
// 테스트 카테고리
// =============================================================================

enum class TestCategory {
    Connectivity,      // API 연결 테스트
    DataFeed,          // 시세 수신 테스트
    Premium,           // 김프 계산 테스트
    Strategy,          // 전략 엔진 테스트
    DryRun,            // Dry-run 거래 테스트
    Infrastructure,    // 인프라 컴포넌트 테스트
    Performance        // 성능 테스트
};

inline const char* to_string(TestCategory cat) {
    switch (cat) {
        case TestCategory::Connectivity:    return "Connectivity";
        case TestCategory::DataFeed:        return "DataFeed";
        case TestCategory::Premium:         return "Premium";
        case TestCategory::Strategy:        return "Strategy";
        case TestCategory::DryRun:          return "DryRun";
        case TestCategory::Infrastructure:  return "Infrastructure";
        case TestCategory::Performance:     return "Performance";
        default:                            return "Unknown";
    }
}

// =============================================================================
// 테스트 설정
// =============================================================================

struct IntegrationTestConfig {
    // 테스트 대상
    bool test_upbit{true};
    bool test_bithumb{true};
    bool test_binance{true};
    bool test_mexc{true};

    // 타임아웃 설정
    int connection_timeout_ms{10000};
    int data_timeout_ms{30000};
    int operation_timeout_ms{60000};

    // 테스트 옵션
    bool skip_slow_tests{false};
    bool verbose{false};
    bool stop_on_failure{false};

    // 성능 기준
    int max_latency_ms{100};
    int min_throughput{100};  // messages/sec
};

// =============================================================================
// 테스트 요약
// =============================================================================

struct TestSummary {
    int total{0};
    int passed{0};
    int failed{0};
    int skipped{0};
    int timeout{0};

    std::chrono::milliseconds total_duration{0};
    std::vector<TestResult> results;

    double pass_rate() const {
        if (total == 0) return 0.0;
        return static_cast<double>(passed) / total * 100.0;
    }

    bool all_passed() const {
        return failed == 0 && timeout == 0;
    }
};

// =============================================================================
// 통합 테스트 러너
// =============================================================================

class IntegrationTestRunner {
public:
    using TestFunc = std::function<TestResult()>;
    using ProgressCallback = std::function<void(const std::string&, TestStatus)>;

    IntegrationTestRunner();
    explicit IntegrationTestRunner(const IntegrationTestConfig& config);
    ~IntegrationTestRunner();

    // =========================================================================
    // 테스트 등록
    // =========================================================================

    /**
     * 테스트 등록
     */
    void register_test(
        const std::string& name,
        TestCategory category,
        TestFunc func
    );

    /**
     * 기본 테스트 등록
     */
    void register_default_tests();

    // =========================================================================
    // 테스트 실행
    // =========================================================================

    /**
     * 모든 테스트 실행
     */
    TestSummary run_all();

    /**
     * 카테고리별 테스트 실행
     */
    TestSummary run_category(TestCategory category);

    /**
     * 단일 테스트 실행
     */
    TestResult run_test(const std::string& name);

    // =========================================================================
    // 설정
    // =========================================================================

    void set_config(const IntegrationTestConfig& config);
    IntegrationTestConfig config() const { return config_; }

    void on_progress(ProgressCallback callback);

    // =========================================================================
    // 결과 출력
    // =========================================================================

    void print_summary(const TestSummary& summary) const;
    void print_result(const TestResult& result) const;

private:
    struct TestEntry {
        std::string name;
        TestCategory category;
        TestFunc func;
    };

    IntegrationTestConfig config_;
    std::vector<TestEntry> tests_;
    ProgressCallback progress_callback_;

    void notify_progress(const std::string& name, TestStatus status);
};

// =============================================================================
// 개별 테스트 함수
// =============================================================================

namespace tests {

// Connectivity Tests (실제 API 연결)
TestResult test_upbit_websocket_connection();
TestResult test_bithumb_websocket_connection();
TestResult test_binance_websocket_connection();
TestResult test_mexc_websocket_connection();

// Data Feed Tests (시세 수신)
TestResult test_upbit_ticker_receive();
TestResult test_bithumb_ticker_receive();
TestResult test_binance_ticker_receive();
TestResult test_mexc_ticker_receive();

// Premium Tests (김프 계산)
TestResult test_premium_calculation();
TestResult test_premium_matrix_update();
TestResult test_fx_rate_integration();

// Strategy Tests (전략 로직)
TestResult test_decision_engine();
TestResult test_strategy_executor();
TestResult test_risk_model();

// Dry-Run Tests (Dry-run 거래)
TestResult test_dual_order_dry_run();
TestResult test_transfer_dry_run();
TestResult test_recovery_dry_run();

// Infrastructure Tests (인프라)
TestResult test_event_bus();
TestResult test_health_check();
TestResult test_shutdown_manager();
TestResult test_daily_limiter();
TestResult test_trading_stats();

// Performance Tests (성능)
TestResult test_spsc_queue_latency();
TestResult test_memory_pool_performance();
TestResult test_json_parser_throughput();

}  // namespace tests

// =============================================================================
// 테스트 헬퍼 매크로
// =============================================================================

#define INTEGRATION_TEST_BEGIN(name) \
    TestResult result; \
    result.name = name; \
    result.timestamp = std::chrono::system_clock::now(); \
    auto start_time = std::chrono::steady_clock::now(); \
    try {

#define INTEGRATION_TEST_END() \
        result.status = TestStatus::Passed; \
        result.message = "OK"; \
    } catch (const std::exception& e) { \
        result.status = TestStatus::Failed; \
        result.message = e.what(); \
    } catch (...) { \
        result.status = TestStatus::Failed; \
        result.message = "Unknown error"; \
    } \
    auto end_time = std::chrono::steady_clock::now(); \
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>( \
        end_time - start_time); \
    return result;

#define INTEGRATION_ASSERT(cond, msg) \
    if (!(cond)) { \
        throw std::runtime_error(msg); \
    }

#define INTEGRATION_SKIP(reason) \
    result.status = TestStatus::Skipped; \
    result.message = reason; \
    return result;

}  // namespace integration
}  // namespace arbitrage
