/**
 * Daily Loss Limit Test (TASK_25)
 *
 * 일일 손실 한도 기능 테스트
 * - 손익 추적
 * - 한도 체크
 * - 킬스위치 연동
 * - 통계
 */

#include "arbitrage/ops/daily_limit.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
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

#define ASSERT_NEAR(a, b, epsilon) \
    if (std::abs((a) - (b)) > (epsilon)) { \
        throw std::runtime_error("Assertion failed: " #a " != " #b " (within " #epsilon ")"); \
    }

// =============================================================================
// 테스트 케이스
// =============================================================================

TEST(daily_stats_defaults) {
    DailyStats stats;

    ASSERT_NEAR(stats.realized_pnl, 0.0, 0.001);
    ASSERT_NEAR(stats.unrealized_pnl, 0.0, 0.001);
    ASSERT_EQ(stats.trade_count, 0);
    ASSERT_EQ(stats.win_count, 0);
    ASSERT_EQ(stats.loss_count, 0);
    ASSERT_NEAR(stats.win_rate(), 0.0, 0.001);
}

TEST(daily_stats_win_rate) {
    DailyStats stats;
    stats.trade_count = 10;
    stats.win_count = 7;
    stats.loss_count = 3;

    ASSERT_NEAR(stats.win_rate(), 70.0, 0.001);
}

TEST(trade_record_construction) {
    TradeRecord record(10000.0, 500000.0);

    ASSERT_NEAR(record.pnl_krw, 10000.0, 0.001);
    ASSERT_NEAR(record.volume_krw, 500000.0, 0.001);
    ASSERT(record.timestamp.time_since_epoch().count() > 0);
}

TEST(config_defaults) {
    DailyLimitConfig config;

    ASSERT(config.daily_loss_limit_krw > 0);
    ASSERT_NEAR(config.warning_threshold_pct, 70.0, 0.001);
    ASSERT_NEAR(config.critical_threshold_pct, 90.0, 0.001);
    ASSERT(config.auto_reset_at_midnight);
    ASSERT(config.enable_kill_switch);
}

TEST(limiter_construction) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 500000.0;

    DailyLossLimiter limiter(config);

    ASSERT(!limiter.is_running());
    ASSERT(!limiter.is_limit_reached());
    ASSERT(limiter.can_trade());
    ASSERT_NEAR(limiter.remaining_limit(), 500000.0, 0.001);
}

TEST(limiter_with_callback) {
    std::atomic<bool> kill_triggered{false};

    DailyLossLimiter limiter(100000.0, [&]() {
        kill_triggered = true;
    });

    ASSERT(!kill_triggered);
    ASSERT(limiter.can_trade());
}

TEST(record_winning_trade) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 100000.0;

    DailyLossLimiter limiter(config);

    // 수익 거래
    bool recorded = limiter.record_trade(5000.0, 100000.0);
    ASSERT(recorded);

    auto stats = limiter.get_stats();
    ASSERT_NEAR(stats.realized_pnl, 5000.0, 0.001);
    ASSERT_EQ(stats.trade_count, 1);
    ASSERT_EQ(stats.win_count, 1);
    ASSERT_EQ(stats.loss_count, 0);
}

TEST(record_losing_trade) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 100000.0;

    DailyLossLimiter limiter(config);

    // 손실 거래
    bool recorded = limiter.record_trade(-3000.0, 100000.0);
    ASSERT(recorded);

    auto stats = limiter.get_stats();
    ASSERT_NEAR(stats.realized_pnl, -3000.0, 0.001);
    ASSERT_EQ(stats.trade_count, 1);
    ASSERT_EQ(stats.win_count, 0);
    ASSERT_EQ(stats.loss_count, 1);
}

TEST(remaining_limit_calculation) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 100000.0;

    DailyLossLimiter limiter(config);

    // 초기 상태
    ASSERT_NEAR(limiter.remaining_limit(), 100000.0, 0.001);

    // 손실 기록
    limiter.record_trade(-30000.0);
    ASSERT_NEAR(limiter.remaining_limit(), 70000.0, 0.001);

    // 추가 손실
    limiter.record_trade(-20000.0);
    ASSERT_NEAR(limiter.remaining_limit(), 50000.0, 0.001);

    // 수익 기록 (손실 상쇄)
    limiter.record_trade(10000.0);
    ASSERT_NEAR(limiter.remaining_limit(), 60000.0, 0.001);
}

TEST(usage_percent) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 100000.0;

    DailyLossLimiter limiter(config);

    // 초기 상태
    ASSERT_NEAR(limiter.usage_percent(), 0.0, 0.001);

    // 50% 손실
    limiter.record_trade(-50000.0);
    ASSERT_NEAR(limiter.usage_percent(), 50.0, 0.001);

    // 80% 손실
    limiter.record_trade(-30000.0);
    ASSERT_NEAR(limiter.usage_percent(), 80.0, 0.001);
}

TEST(limit_reached) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 100000.0;
    config.enable_kill_switch = false;  // 킬스위치 비활성화

    DailyLossLimiter limiter(config);

    ASSERT(!limiter.is_limit_reached());
    ASSERT(limiter.can_trade());

    // 한도 도달
    limiter.record_trade(-100000.0);

    ASSERT(limiter.is_limit_reached());
    ASSERT(!limiter.can_trade());
}

TEST(kill_switch_trigger) {
    std::atomic<bool> kill_triggered{false};

    DailyLimitConfig config;
    config.daily_loss_limit_krw = 50000.0;
    config.enable_kill_switch = true;

    DailyLossLimiter limiter(config);
    limiter.set_kill_switch([&]() {
        kill_triggered = true;
    });

    ASSERT(!kill_triggered);

    // 한도 도달
    limiter.record_trade(-50000.0);

    ASSERT(kill_triggered);
    ASSERT(limiter.is_limit_reached());
}

TEST(warning_callback) {
    std::atomic<bool> warning_triggered{false};
    double captured_loss = 0.0;
    double captured_pct = 0.0;

    DailyLimitConfig config;
    config.daily_loss_limit_krw = 100000.0;
    config.warning_threshold_pct = 70.0;
    config.enable_kill_switch = false;

    DailyLossLimiter limiter(config);
    limiter.on_warning([&](double loss, double limit, double pct) {
        warning_triggered = true;
        captured_loss = loss;
        captured_pct = pct;
    });

    // 60% 손실 - 경고 미발생
    limiter.record_trade(-60000.0);
    ASSERT(!warning_triggered);

    // 75% 손실 - 경고 발생
    limiter.record_trade(-15000.0);
    ASSERT(warning_triggered);
    ASSERT_NEAR(captured_loss, 75000.0, 0.001);
    ASSERT_NEAR(captured_pct, 75.0, 0.001);
}

TEST(critical_callback) {
    std::atomic<bool> critical_triggered{false};

    DailyLimitConfig config;
    config.daily_loss_limit_krw = 100000.0;
    config.critical_threshold_pct = 90.0;
    config.enable_kill_switch = false;

    DailyLossLimiter limiter(config);
    limiter.on_critical([&](double, double, double) {
        critical_triggered = true;
    });

    // 85% 손실 - 위험 미발생
    limiter.record_trade(-85000.0);
    ASSERT(!critical_triggered);

    // 95% 손실 - 위험 발생
    limiter.record_trade(-10000.0);
    ASSERT(critical_triggered);
}

TEST(reset) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 100000.0;
    config.enable_kill_switch = false;

    DailyLossLimiter limiter(config);

    // 거래 기록
    limiter.record_trade(-50000.0);
    limiter.record_trade(10000.0);
    limiter.record_trade(-60000.0);  // 한도 도달

    ASSERT(limiter.is_limit_reached());
    ASSERT_EQ(limiter.get_stats().trade_count, 3);

    // 리셋
    limiter.reset();

    ASSERT(!limiter.is_limit_reached());
    ASSERT(limiter.can_trade());
    ASSERT_EQ(limiter.get_stats().trade_count, 0);
    ASSERT_NEAR(limiter.remaining_limit(), 100000.0, 0.001);
}

TEST(cannot_trade_after_limit) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 50000.0;
    config.enable_kill_switch = false;

    DailyLossLimiter limiter(config);

    // 한도 도달
    limiter.record_trade(-50000.0);
    ASSERT(limiter.is_limit_reached());

    // 추가 거래 시도 - 실패해야 함
    bool recorded = limiter.record_trade(-10000.0);
    ASSERT(!recorded);

    // 통계는 변경되지 않아야 함
    auto stats = limiter.get_stats();
    ASSERT_EQ(stats.trade_count, 1);
    ASSERT_NEAR(stats.realized_pnl, -50000.0, 0.001);
}

TEST(trade_history) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 1000000.0;
    config.max_trade_history = 100;

    DailyLossLimiter limiter(config);

    for (int i = 0; i < 10; ++i) {
        limiter.record_trade(1000.0 * (i % 2 == 0 ? 1 : -1));
    }

    auto history = limiter.get_trade_history();
    ASSERT_EQ(history.size(), 10);
    ASSERT_EQ(limiter.trade_history_size(), 10);
}

TEST(trade_history_max_size) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 1000000.0;
    config.max_trade_history = 5;

    DailyLossLimiter limiter(config);

    for (int i = 0; i < 10; ++i) {
        limiter.record_trade(1000.0);
    }

    // 최대 5개만 유지
    ASSERT_EQ(limiter.trade_history_size(), 5);
}

TEST(unrealized_pnl) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 100000.0;
    config.track_unrealized = true;

    DailyLossLimiter limiter(config);

    // 실현 손익
    limiter.record_trade(-30000.0);

    // 미실현 손익 업데이트
    limiter.update_unrealized_pnl(-20000.0);

    auto stats = limiter.get_stats();
    ASSERT_NEAR(stats.realized_pnl, -30000.0, 0.001);
    ASSERT_NEAR(stats.unrealized_pnl, -20000.0, 0.001);
    ASSERT_NEAR(stats.total_pnl, -50000.0, 0.001);
}

TEST(max_drawdown_tracking) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 1000000.0;

    DailyLossLimiter limiter(config);

    // 수익 → 손실 → 수익 패턴
    limiter.record_trade(50000.0);  // peak: 50000
    limiter.record_trade(-30000.0); // now: 20000, drawdown: 30000
    limiter.record_trade(-40000.0); // now: -20000, drawdown: 70000
    limiter.record_trade(30000.0);  // now: 10000

    auto stats = limiter.get_stats();
    ASSERT_NEAR(stats.peak_pnl, 50000.0, 0.001);
    ASSERT_NEAR(stats.max_drawdown, 70000.0, 0.001);
}

TEST(largest_win_loss) {
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 1000000.0;

    DailyLossLimiter limiter(config);

    limiter.record_trade(10000.0);
    limiter.record_trade(50000.0);
    limiter.record_trade(-20000.0);
    limiter.record_trade(-80000.0);
    limiter.record_trade(30000.0);

    auto stats = limiter.get_stats();
    ASSERT_NEAR(stats.largest_win, 50000.0, 0.001);
    ASSERT_NEAR(stats.largest_loss, -80000.0, 0.001);
}

TEST(start_stop) {
    DailyLimitConfig config;
    config.auto_reset_at_midnight = true;

    DailyLossLimiter limiter(config);

    limiter.start();
    ASSERT(limiter.is_running());

    limiter.stop();
    ASSERT(!limiter.is_running());
}

TEST(next_reset_time) {
    DailyLimitConfig config;
    config.reset_hour_kst = 0;  // 자정

    DailyLossLimiter limiter(config);

    auto next_reset = limiter.next_reset_time();
    auto now = std::chrono::system_clock::now();

    // 다음 리셋은 미래여야 함
    ASSERT(next_reset > now);

    // 24시간 이내여야 함
    auto diff = next_reset - now;
    ASSERT(diff <= std::chrono::hours(24));
}

TEST(global_functions) {
    // 글로벌 함수 테스트
    DailyLimitConfig config;
    config.daily_loss_limit_krw = 1000000.0;
    daily_limiter().set_config(config);
    daily_limiter().reset();

    ASSERT(can_trade_today());
    ASSERT_NEAR(remaining_daily_limit(), 1000000.0, 0.001);

    bool recorded = record_daily_trade(-100000.0);
    ASSERT(recorded);
    ASSERT_NEAR(remaining_daily_limit(), 900000.0, 0.001);
}

// =============================================================================
// 메인
// =============================================================================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "  Daily Loss Limit Test (TASK_25)\n";
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
