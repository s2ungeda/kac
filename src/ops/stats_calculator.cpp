/**
 * Stats Calculator Implementation (refactored from trading_stats.cpp)
 *
 * Pure calculation logic — no I/O.
 */

#include "arbitrage/ops/stats_calculator.hpp"
#include "arbitrage/ops/trading_stats.hpp"

#include <cmath>
#include <ctime>

namespace arbitrage {

// =============================================================================
// 통계 계산
// =============================================================================

TradingStats StatsCalculator::calculate_stats(
    const std::vector<ExtendedTradeRecord>& trades,
    StatsPeriod period
) const {
    TradingStats stats;
    stats.period = period;

    if (trades.empty()) {
        return stats;
    }

    stats.start_time = trades.front().timestamp;
    stats.end_time = trades.back().timestamp;

    double cumulative_pnl = 0.0;
    double peak = 0.0;
    int streak = 0;

    for (const auto& trade : trades) {
        stats.total_trades++;
        double pnl = trade.net_pnl_krw;
        stats.net_pnl_krw += pnl;
        stats.total_volume_krw += trade.volume_krw();

        if (pnl > 0) {
            stats.winning_trades++;
            stats.total_profit_krw += pnl;
            if (pnl > stats.largest_win_krw) {
                stats.largest_win_krw = pnl;
            }
            if (streak >= 0) {
                streak++;
            } else {
                streak = 1;
            }
        } else if (pnl < 0) {
            stats.losing_trades++;
            stats.total_loss_krw += std::abs(pnl);
            if (std::abs(pnl) > stats.largest_loss_krw) {
                stats.largest_loss_krw = std::abs(pnl);
            }
            if (streak <= 0) {
                streak--;
            } else {
                streak = -1;
            }
        } else {
            stats.break_even_trades++;
        }

        // 연속 기록
        if (streak > stats.max_consecutive_wins) {
            stats.max_consecutive_wins = streak;
        }
        if (streak < 0 && (-streak) > stats.max_consecutive_losses) {
            stats.max_consecutive_losses = -streak;
        }

        // 드로다운
        cumulative_pnl += pnl;
        if (cumulative_pnl > peak) {
            peak = cumulative_pnl;
        }
        double dd = peak - cumulative_pnl;
        if (dd > stats.max_drawdown_krw) {
            stats.max_drawdown_krw = dd;
        }
    }

    stats.current_streak = streak;

    // 평균 계산
    if (stats.winning_trades > 0) {
        stats.avg_win_krw = stats.total_profit_krw / stats.winning_trades;
    }
    if (stats.losing_trades > 0) {
        stats.avg_loss_krw = stats.total_loss_krw / stats.losing_trades;
    }

    // 드로다운 퍼센트
    if (peak > 0) {
        stats.max_drawdown_pct = stats.max_drawdown_krw / peak * 100.0;
    }
    stats.peak_equity_krw = peak;

    return stats;
}

// =============================================================================
// 드로다운 업데이트
// =============================================================================

void StatsCalculator::update_drawdown(
    TradingStats& all_time_stats,
    double max_drawdown_krw,
    double max_drawdown_pct
) {
    // current_equity는 외부에서 업데이트됨
    // 여기서는 통계만 업데이트
    all_time_stats.max_drawdown_krw = max_drawdown_krw;
    all_time_stats.max_drawdown_pct = max_drawdown_pct;
}

// =============================================================================
// 연속 기록 업데이트
// =============================================================================

void StatsCalculator::update_streak(
    bool is_win,
    int& current_streak,
    int& max_consecutive_wins,
    int& max_consecutive_losses,
    TradingStats& all_time_stats
) {
    if (is_win) {
        if (current_streak >= 0) {
            current_streak++;
        } else {
            current_streak = 1;
        }
        if (current_streak > max_consecutive_wins) {
            max_consecutive_wins = current_streak;
        }
    } else {
        if (current_streak <= 0) {
            current_streak--;
        } else {
            current_streak = -1;
        }
        if ((-current_streak) > max_consecutive_losses) {
            max_consecutive_losses = -current_streak;
        }
    }

    all_time_stats.current_streak = current_streak;
    all_time_stats.max_consecutive_wins = max_consecutive_wins;
    all_time_stats.max_consecutive_losses = max_consecutive_losses;
}

// =============================================================================
// 날짜 비교 헬퍼
// =============================================================================

bool StatsCalculator::is_same_day(
    std::chrono::system_clock::time_point t1,
    std::chrono::system_clock::time_point t2
) const {
    auto time1 = std::chrono::system_clock::to_time_t(t1);
    auto time2 = std::chrono::system_clock::to_time_t(t2);
    std::tm tm1 = *std::localtime(&time1);
    std::tm tm2 = *std::localtime(&time2);

    return tm1.tm_year == tm2.tm_year &&
           tm1.tm_mon == tm2.tm_mon &&
           tm1.tm_mday == tm2.tm_mday;
}

bool StatsCalculator::is_same_week(
    std::chrono::system_clock::time_point t1,
    std::chrono::system_clock::time_point t2
) const {
    auto time1 = std::chrono::system_clock::to_time_t(t1);
    auto time2 = std::chrono::system_clock::to_time_t(t2);
    std::tm tm1 = *std::localtime(&time1);
    std::tm tm2 = *std::localtime(&time2);

    // ISO 주 번호 비교
    char buf1[3], buf2[3];
    std::strftime(buf1, 3, "%V", &tm1);
    std::strftime(buf2, 3, "%V", &tm2);

    return tm1.tm_year == tm2.tm_year && std::string(buf1) == std::string(buf2);
}

bool StatsCalculator::is_same_month(
    std::chrono::system_clock::time_point t1,
    std::chrono::system_clock::time_point t2
) const {
    auto time1 = std::chrono::system_clock::to_time_t(t1);
    auto time2 = std::chrono::system_clock::to_time_t(t2);
    std::tm tm1 = *std::localtime(&time1);
    std::tm tm2 = *std::localtime(&time2);

    return tm1.tm_year == tm2.tm_year && tm1.tm_mon == tm2.tm_mon;
}

}  // namespace arbitrage
