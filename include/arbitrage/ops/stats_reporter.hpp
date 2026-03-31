#pragma once

/**
 * Stats Reporter (refactored from trading_stats)
 *
 * CSV file I/O, daily/weekly/monthly aggregation formatting, report generation.
 * No business logic — purely I/O and formatting.
 */

#include <chrono>
#include <deque>
#include <string>
#include <vector>

#include "arbitrage/common/spin_wait.hpp"

namespace arbitrage {

// Forward declarations
struct TradingStatsConfig;
struct ExtendedTradeRecord;
struct DailySummary;
struct TradingStats;
enum class StatsPeriod;
class StatsCalculator;

// =============================================================================
// StatsReporter
// =============================================================================

class StatsReporter {
public:
    StatsReporter() = default;
    ~StatsReporter() = default;

    StatsReporter(const StatsReporter&) = delete;
    StatsReporter& operator=(const StatsReporter&) = delete;

    // =========================================================================
    // 파일 저장
    // =========================================================================

    /**
     * 거래 기록을 CSV로 저장
     */
    bool save_trades(
        const TradingStatsConfig& config,
        const std::deque<ExtendedTradeRecord>& trades,
        RWSpinLock& trades_mutex
    );

    /**
     * 일별 통계를 CSV로 저장
     */
    bool save_daily_stats(
        const TradingStatsConfig& config,
        const std::deque<ExtendedTradeRecord>& trades,
        RWSpinLock& trades_mutex
    );

    // =========================================================================
    // 파일 로드
    // =========================================================================

    /**
     * 거래 기록 CSV에서 로드
     * @return 로드 성공 여부
     */
    bool load_trades(
        const TradingStatsConfig& config,
        std::deque<ExtendedTradeRecord>& trades,
        size_t& total_trades_ever,
        RWSpinLock& trades_mutex,
        TradingStats& all_time_stats,
        RWSpinLock& stats_mutex,
        const StatsCalculator& calculator
    );

    // =========================================================================
    // 집계 포맷팅
    // =========================================================================

    /**
     * 최근 N일 일별 요약
     */
    std::vector<DailySummary> get_daily_summaries(
        int days,
        const std::deque<ExtendedTradeRecord>& trades,
        RWSpinLock& trades_mutex
    ) const;

    /**
     * 월별 요약
     */
    std::vector<DailySummary> get_monthly_summaries(
        int months,
        const std::deque<ExtendedTradeRecord>& trades,
        RWSpinLock& trades_mutex
    ) const;

    // =========================================================================
    // CSV 파싱 헬퍼
    // =========================================================================

    std::vector<std::string> parse_csv_line(const std::string& line) const;
};

}  // namespace arbitrage
