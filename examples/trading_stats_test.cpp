/**
 * Trading Stats Test (TASK_28)
 *
 * 거래 통계 기능 테스트
 * - TradingStats 구조체
 * - TradingStatsTracker 클래스
 * - 일/주/월/전체 통계
 * - 파일 저장/로드
 */

#include "arbitrage/ops/trading_stats.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
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
// TradingStats 구조체 테스트
// =============================================================================

TEST(trading_stats_defaults) {
    TradingStats stats;

    ASSERT_EQ(stats.total_trades, 0);
    ASSERT_EQ(stats.winning_trades, 0);
    ASSERT_EQ(stats.losing_trades, 0);
    ASSERT_NEAR(stats.net_pnl_krw, 0.0, 0.001);
    ASSERT_NEAR(stats.win_rate(), 0.0, 0.001);
}

TEST(trading_stats_win_rate) {
    TradingStats stats;
    stats.total_trades = 100;
    stats.winning_trades = 60;
    stats.losing_trades = 40;

    ASSERT_NEAR(stats.win_rate(), 60.0, 0.001);
}

TEST(trading_stats_profit_factor) {
    TradingStats stats;
    stats.winning_trades = 10;
    stats.losing_trades = 5;
    stats.total_profit_krw = 100000.0;  // 10만원 수익
    stats.total_loss_krw = 50000.0;     // 5만원 손실

    // Profit Factor = 10만 / 5만 = 2.0
    ASSERT_NEAR(stats.profit_factor(), 2.0, 0.001);
}

TEST(trading_stats_expectancy) {
    TradingStats stats;
    stats.total_trades = 10;
    stats.net_pnl_krw = 50000.0;  // 5만원 순수익

    // 기대값 = 5만 / 10 = 5000원
    ASSERT_NEAR(stats.expectancy(), 5000.0, 0.001);
}

TEST(trading_stats_risk_reward) {
    TradingStats stats;
    stats.winning_trades = 10;
    stats.losing_trades = 10;
    stats.total_profit_krw = 100000.0;
    stats.total_loss_krw = 50000.0;

    // 평균 수익 = 10000, 평균 손실 = 5000
    // R:R = 10000 / 5000 = 2.0
    ASSERT_NEAR(stats.risk_reward_ratio(), 2.0, 0.001);
}

TEST(trading_stats_avg_profit_loss) {
    TradingStats stats;
    stats.winning_trades = 5;
    stats.losing_trades = 3;
    stats.total_profit_krw = 50000.0;
    stats.total_loss_krw = 15000.0;

    // 평균 수익 = 50000 / 5 = 10000
    ASSERT_NEAR(stats.avg_profit(), 10000.0, 0.001);

    // 평균 손실 = 15000 / 3 = 5000
    ASSERT_NEAR(stats.avg_loss(), 5000.0, 0.001);
}

TEST(trading_stats_sharpe_ratio_empty) {
    TradingStats stats;
    // 일별 수익률 없음
    ASSERT_NEAR(stats.sharpe_ratio(), 0.0, 0.001);
}

TEST(trading_stats_sharpe_ratio) {
    TradingStats stats;
    // 10일간 일별 수익률 (%)
    stats.daily_returns = {0.01, 0.02, -0.005, 0.015, 0.008,
                           0.01, -0.003, 0.012, 0.009, 0.011};

    double sharpe = stats.sharpe_ratio();
    // 양수여야 함 (평균 수익 > 무위험 수익률)
    ASSERT(sharpe > 0);
}

TEST(trading_stats_max_drawdown) {
    TradingStats stats;
    stats.max_drawdown_pct = 15.5;

    ASSERT_NEAR(stats.max_drawdown(), 15.5, 0.001);
}

// =============================================================================
// ExtendedTradeRecord 테스트
// =============================================================================

TEST(extended_trade_record_defaults) {
    ExtendedTradeRecord record;

    ASSERT(record.trade_id.empty());
    ASSERT_NEAR(record.net_pnl_krw, 0.0, 0.001);
    ASSERT_NEAR(record.quantity, 0.0, 0.001);
}

TEST(extended_trade_record_volume) {
    ExtendedTradeRecord record;
    record.buy_price = 3000.0;
    record.sell_price = 3100.0;
    record.quantity = 100.0;

    // 거래량 = (3000 * 100) + (3100 * 100) = 610000
    ASSERT_NEAR(record.volume_krw(), 610000.0, 0.001);
}

TEST(extended_trade_record_fees) {
    ExtendedTradeRecord record;
    record.buy_fee_krw = 100.0;
    record.sell_fee_krw = 150.0;
    record.transfer_fee_krw = 50.0;

    ASSERT_NEAR(record.total_fees(), 300.0, 0.001);
}

// =============================================================================
// DailySummary 테스트
// =============================================================================

TEST(daily_summary_return_pct) {
    DailySummary summary;
    summary.equity_start = 10000000.0;  // 1000만원
    summary.equity_end = 10100000.0;    // 1010만원

    // 수익률 = (1010만 - 1000만) / 1000만 * 100 = 1%
    ASSERT_NEAR(summary.return_pct(), 1.0, 0.001);
}

TEST(daily_summary_negative_return) {
    DailySummary summary;
    summary.equity_start = 10000000.0;
    summary.equity_end = 9500000.0;  // 5% 손실

    ASSERT_NEAR(summary.return_pct(), -5.0, 0.001);
}

// =============================================================================
// TradingStatsConfig 테스트
// =============================================================================

TEST(config_defaults) {
    TradingStatsConfig config;

    ASSERT_EQ(config.data_dir, "data/stats");
    ASSERT_EQ(config.trades_file, "trades.csv");
    ASSERT_EQ(config.max_trades_in_memory, 10000);
    ASSERT(config.auto_save);
    ASSERT_NEAR(config.initial_capital_krw, 10000000.0, 0.001);
}

// =============================================================================
// TradingStatsTracker 테스트
// =============================================================================

TEST(tracker_construction) {
    TradingStatsTracker tracker;

    ASSERT(!tracker.is_running());
    ASSERT_EQ(tracker.total_trade_count(), 0);
}

TEST(tracker_with_config) {
    TradingStatsConfig config;
    config.initial_capital_krw = 5000000.0;
    config.data_dir = "/tmp/test_stats";

    TradingStatsTracker tracker(config);

    ASSERT_EQ(tracker.config().data_dir, "/tmp/test_stats");
}

TEST(tracker_record_trade_simple) {
    TradingStatsTracker tracker;

    tracker.record_trade(10000.0, 100000.0);  // 1만원 수익, 10만원 거래

    auto stats = tracker.get_all_time_stats();
    ASSERT_EQ(stats.total_trades, 1);
    ASSERT_EQ(stats.winning_trades, 1);
    ASSERT_NEAR(stats.net_pnl_krw, 10000.0, 0.001);
}

TEST(tracker_record_trade_loss) {
    TradingStatsTracker tracker;

    tracker.record_trade(-5000.0);  // 5천원 손실

    auto stats = tracker.get_all_time_stats();
    ASSERT_EQ(stats.total_trades, 1);
    ASSERT_EQ(stats.losing_trades, 1);
    ASSERT_NEAR(stats.net_pnl_krw, -5000.0, 0.001);
    ASSERT_NEAR(stats.total_loss_krw, 5000.0, 0.001);
}

TEST(tracker_record_multiple_trades) {
    TradingStatsTracker tracker;

    tracker.record_trade(10000.0);   // 수익
    tracker.record_trade(-3000.0);   // 손실
    tracker.record_trade(5000.0);    // 수익
    tracker.record_trade(-2000.0);   // 손실
    tracker.record_trade(8000.0);    // 수익

    auto stats = tracker.get_all_time_stats();
    ASSERT_EQ(stats.total_trades, 5);
    ASSERT_EQ(stats.winning_trades, 3);
    ASSERT_EQ(stats.losing_trades, 2);
    ASSERT_NEAR(stats.net_pnl_krw, 18000.0, 0.001);  // 10-3+5-2+8 = 18
    ASSERT_NEAR(stats.total_profit_krw, 23000.0, 0.001);
    ASSERT_NEAR(stats.total_loss_krw, 5000.0, 0.001);
}

TEST(tracker_record_extended_trade) {
    TradingStatsTracker tracker;

    ExtendedTradeRecord record;
    record.trade_id = "TEST001";
    record.buy_exchange = "Binance";
    record.sell_exchange = "Upbit";
    record.buy_price = 2.15;
    record.sell_price = 3200.0;
    record.quantity = 1000.0;
    record.net_pnl_krw = 50000.0;
    record.premium_pct = 2.5;
    record.strategy_name = "BasicArb";

    tracker.record_trade(record);

    auto recent = tracker.get_recent_trades(1);
    ASSERT_EQ(recent.size(), 1);
    ASSERT_EQ(recent[0].trade_id, "TEST001");
    ASSERT_EQ(recent[0].buy_exchange, "Binance");
    ASSERT_NEAR(recent[0].premium_pct, 2.5, 0.001);
}

TEST(tracker_win_rate_calculation) {
    TradingStatsTracker tracker;

    for (int i = 0; i < 6; i++) tracker.record_trade(1000.0);   // 6 wins
    for (int i = 0; i < 4; i++) tracker.record_trade(-500.0);   // 4 losses

    auto stats = tracker.get_all_time_stats();
    ASSERT_NEAR(stats.win_rate(), 60.0, 0.001);
}

TEST(tracker_largest_win_loss) {
    TradingStatsTracker tracker;

    tracker.record_trade(5000.0);
    tracker.record_trade(15000.0);   // 최대 수익
    tracker.record_trade(8000.0);
    tracker.record_trade(-3000.0);
    tracker.record_trade(-10000.0);  // 최대 손실
    tracker.record_trade(-2000.0);

    auto stats = tracker.get_all_time_stats();
    ASSERT_NEAR(stats.largest_win_krw, 15000.0, 0.001);
    ASSERT_NEAR(stats.largest_loss_krw, 10000.0, 0.001);
}

TEST(tracker_consecutive_wins) {
    TradingStatsTracker tracker;

    // 3연승 -> 1패 -> 2연승
    tracker.record_trade(1000.0);
    tracker.record_trade(1000.0);
    tracker.record_trade(1000.0);
    tracker.record_trade(-500.0);
    tracker.record_trade(1000.0);
    tracker.record_trade(1000.0);

    auto stats = tracker.get_all_time_stats();
    ASSERT_EQ(stats.max_consecutive_wins, 3);
}

TEST(tracker_consecutive_losses) {
    TradingStatsTracker tracker;

    // 1승 -> 4연패 -> 1승
    tracker.record_trade(1000.0);
    tracker.record_trade(-500.0);
    tracker.record_trade(-500.0);
    tracker.record_trade(-500.0);
    tracker.record_trade(-500.0);
    tracker.record_trade(1000.0);

    auto stats = tracker.get_all_time_stats();
    ASSERT_EQ(stats.max_consecutive_losses, 4);
}

TEST(tracker_get_recent_trades) {
    TradingStatsTracker tracker;

    for (int i = 0; i < 10; i++) {
        tracker.record_trade(1000.0 * (i + 1));
    }

    auto recent = tracker.get_recent_trades(5);
    ASSERT_EQ(recent.size(), 5);

    // 최근 5개는 6번째~10번째 거래
    ASSERT_NEAR(recent[0].net_pnl_krw, 6000.0, 0.001);
    ASSERT_NEAR(recent[4].net_pnl_krw, 10000.0, 0.001);
}

TEST(tracker_daily_stats) {
    TradingStatsTracker tracker;

    // 오늘 거래 기록
    tracker.record_trade(5000.0);
    tracker.record_trade(-2000.0);
    tracker.record_trade(3000.0);

    auto daily = tracker.get_daily_stats();
    ASSERT_EQ(daily.total_trades, 3);
    ASSERT_NEAR(daily.net_pnl_krw, 6000.0, 0.001);
}

TEST(tracker_update_equity) {
    TradingStatsTracker tracker;

    tracker.update_equity(10000000.0);  // 1000만원
    tracker.update_equity(10500000.0);  // 1050만원 (새 고점)
    tracker.update_equity(10200000.0);  // 1020만원 (30만원 드로다운)

    auto stats = tracker.get_all_time_stats();
    ASSERT_NEAR(stats.peak_equity_krw, 10500000.0, 0.001);
}

// =============================================================================
// 파일 저장/로드 테스트
// =============================================================================

TEST(tracker_save_trades) {
    namespace fs = std::filesystem;

    // 테스트 디렉토리 설정
    TradingStatsConfig config;
    config.data_dir = "/tmp/trading_stats_test";
    config.auto_save = false;

    // 기존 파일 삭제
    fs::remove_all(config.data_dir);

    TradingStatsTracker tracker(config);

    ExtendedTradeRecord record;
    record.trade_id = "SAVE001";
    record.buy_exchange = "Binance";
    record.sell_exchange = "Upbit";
    record.net_pnl_krw = 25000.0;
    tracker.record_trade(record);

    bool saved = tracker.save_trades();
    ASSERT(saved);

    // 파일 존재 확인
    std::string path = config.data_dir + "/" + config.trades_file;
    ASSERT(fs::exists(path));

    // 정리
    fs::remove_all(config.data_dir);
}

TEST(tracker_save_daily_stats) {
    namespace fs = std::filesystem;

    TradingStatsConfig config;
    config.data_dir = "/tmp/trading_stats_test2";
    config.auto_save = false;

    fs::remove_all(config.data_dir);

    TradingStatsTracker tracker(config);

    tracker.record_trade(10000.0);
    tracker.record_trade(-5000.0);

    bool saved = tracker.save_daily_stats();
    ASSERT(saved);

    std::string path = config.data_dir + "/" + config.daily_stats_file;
    ASSERT(fs::exists(path));

    fs::remove_all(config.data_dir);
}

TEST(tracker_load_trades) {
    namespace fs = std::filesystem;

    TradingStatsConfig config;
    config.data_dir = "/tmp/trading_stats_test3";
    config.auto_save = false;

    fs::remove_all(config.data_dir);

    // 첫 번째 트래커에서 저장
    {
        TradingStatsTracker tracker(config);
        tracker.record_trade(10000.0);
        tracker.record_trade(-3000.0);
        tracker.record_trade(5000.0);
        tracker.save_trades();
    }

    // 두 번째 트래커에서 로드
    {
        TradingStatsTracker tracker(config);
        bool loaded = tracker.load_trades();
        ASSERT(loaded);

        ASSERT_EQ(tracker.total_trade_count(), 3);

        auto stats = tracker.get_all_time_stats();
        ASSERT_EQ(stats.total_trades, 3);
        ASSERT_NEAR(stats.net_pnl_krw, 12000.0, 0.001);
    }

    fs::remove_all(config.data_dir);
}

// =============================================================================
// 리셋 테스트
// =============================================================================

TEST(tracker_reset_stats) {
    TradingStatsTracker tracker;

    tracker.record_trade(10000.0);
    tracker.record_trade(-5000.0);

    tracker.reset_stats();

    auto stats = tracker.get_all_time_stats();
    ASSERT_EQ(stats.total_trades, 0);
    ASSERT_NEAR(stats.net_pnl_krw, 0.0, 0.001);

    // 거래 기록은 유지
    ASSERT_EQ(tracker.total_trade_count(), 2);
}

TEST(tracker_reset_all) {
    TradingStatsTracker tracker;

    tracker.record_trade(10000.0);
    tracker.record_trade(-5000.0);

    tracker.reset_all();

    auto stats = tracker.get_all_time_stats();
    ASSERT_EQ(stats.total_trades, 0);
    ASSERT_EQ(tracker.total_trade_count(), 0);
}

// =============================================================================
// 일별 요약 테스트
// =============================================================================

TEST(tracker_daily_summaries) {
    TradingStatsTracker tracker;

    tracker.record_trade(10000.0);
    tracker.record_trade(-5000.0);
    tracker.record_trade(8000.0);

    auto summaries = tracker.get_daily_summaries(7);
    // 오늘 거래만 있으므로 1개
    ASSERT(summaries.size() >= 1);

    auto& today = summaries.back();
    ASSERT_EQ(today.trade_count, 3);
}

// =============================================================================
// 서비스 제어 테스트
// =============================================================================

TEST(tracker_start_stop) {
    TradingStatsConfig config;
    config.data_dir = "/tmp/trading_stats_test4";
    config.auto_save = true;
    config.auto_save_interval_sec = 1;

    namespace fs = std::filesystem;
    fs::remove_all(config.data_dir);

    TradingStatsTracker tracker(config);

    tracker.start();
    ASSERT(tracker.is_running());

    tracker.record_trade(5000.0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    tracker.stop();
    ASSERT(!tracker.is_running());

    fs::remove_all(config.data_dir);
}

// =============================================================================
// 글로벌 접근자 테스트
// =============================================================================

TEST(global_accessor) {
    auto& tracker1 = trading_stats();
    auto& tracker2 = trading_stats();

    // 같은 인스턴스
    ASSERT(&tracker1 == &tracker2);
}

// =============================================================================
// StatsPeriod 문자열 변환 테스트
// =============================================================================

TEST(stats_period_to_string) {
    ASSERT_EQ(std::string(to_string(StatsPeriod::Daily)), "Daily");
    ASSERT_EQ(std::string(to_string(StatsPeriod::Weekly)), "Weekly");
    ASSERT_EQ(std::string(to_string(StatsPeriod::Monthly)), "Monthly");
    ASSERT_EQ(std::string(to_string(StatsPeriod::Yearly)), "Yearly");
    ASSERT_EQ(std::string(to_string(StatsPeriod::AllTime)), "AllTime");
}

// =============================================================================
// 메인
// =============================================================================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "  Trading Stats Test (TASK_28)\n";
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
