#pragma once

/**
 * Stats Calculator (refactored from trading_stats)
 *
 * Pure calculation logic — no I/O.
 * - calculate_stats from trade records
 * - Drawdown tracking
 * - Streak tracking
 * - Date comparison helpers
 */

#include "arbitrage/ops/daily_limit.hpp"

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace arbitrage {

// Forward declarations
struct TradingStats;
struct ExtendedTradeRecord;
enum class StatsPeriod;

// =============================================================================
// StatsCalculator
// =============================================================================

class StatsCalculator {
public:
    StatsCalculator() = default;
    ~StatsCalculator() = default;

    StatsCalculator(const StatsCalculator&) = delete;
    StatsCalculator& operator=(const StatsCalculator&) = delete;

    // =========================================================================
    // 통계 계산
    // =========================================================================

    /**
     * 거래 기록에서 통계 계산 (기간별)
     */
    TradingStats calculate_stats(
        const std::vector<ExtendedTradeRecord>& trades,
        StatsPeriod period
    ) const;

    // =========================================================================
    // 드로다운 업데이트
    // =========================================================================

    /**
     * 드로다운 업데이트 (all_time_stats 반영)
     */
    void update_drawdown(
        TradingStats& all_time_stats,
        double max_drawdown_krw,
        double max_drawdown_pct
    );

    // =========================================================================
    // 연속 기록 업데이트
    // =========================================================================

    /**
     * 연속 기록 업데이트
     * @return 업데이트된 현재 streak 값
     */
    void update_streak(
        bool is_win,
        int& current_streak,
        int& max_consecutive_wins,
        int& max_consecutive_losses,
        TradingStats& all_time_stats
    );

    // =========================================================================
    // 날짜 비교 헬퍼
    // =========================================================================

    bool is_same_day(
        std::chrono::system_clock::time_point t1,
        std::chrono::system_clock::time_point t2
    ) const;

    bool is_same_week(
        std::chrono::system_clock::time_point t1,
        std::chrono::system_clock::time_point t2
    ) const;

    bool is_same_month(
        std::chrono::system_clock::time_point t1,
        std::chrono::system_clock::time_point t2
    ) const;
};

}  // namespace arbitrage
