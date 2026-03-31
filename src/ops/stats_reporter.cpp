/**
 * Stats Reporter Implementation (refactored from trading_stats.cpp)
 *
 * CSV file I/O, daily/weekly/monthly aggregation formatting.
 */

#include "arbitrage/ops/stats_reporter.hpp"
#include "arbitrage/ops/trading_stats.hpp"
#include "arbitrage/ops/stats_calculator.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace arbitrage {

// =============================================================================
// 파일 저장
// =============================================================================

bool StatsReporter::save_trades(
    const TradingStatsConfig& config,
    const std::deque<ExtendedTradeRecord>& trades,
    RWSpinLock& trades_mutex
) {
    try {
        namespace fs = std::filesystem;
        fs::create_directories(config.data_dir);

        std::string path = config.data_dir + "/" + config.trades_file;
        std::ofstream file(path);
        if (!file.is_open()) {
            return false;
        }

        // 헤더
        file << "timestamp,trade_id,buy_exchange,sell_exchange,symbol,"
             << "buy_price,sell_price,quantity,"
             << "buy_fee,sell_fee,transfer_fee,"
             << "gross_pnl,net_pnl,premium_pct,"
             << "strategy,notes\n";

        ReadGuard lock(trades_mutex);

        for (const auto& trade : trades) {
            auto t = std::chrono::system_clock::to_time_t(trade.timestamp);
            std::tm tm = *std::localtime(&t);

            file << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << ","
                 << trade.trade_id << ","
                 << trade.buy_exchange << ","
                 << trade.sell_exchange << ","
                 << trade.symbol << ","
                 << std::fixed << std::setprecision(2)
                 << trade.buy_price << ","
                 << trade.sell_price << ","
                 << std::setprecision(6)
                 << trade.quantity << ","
                 << std::setprecision(2)
                 << trade.buy_fee_krw << ","
                 << trade.sell_fee_krw << ","
                 << trade.transfer_fee_krw << ","
                 << trade.gross_pnl_krw << ","
                 << trade.net_pnl_krw << ","
                 << std::setprecision(4)
                 << trade.premium_pct << ","
                 << trade.strategy_name << ","
                 << trade.notes << "\n";
        }

        return true;
    } catch (...) {
        return false;
    }
}

bool StatsReporter::save_daily_stats(
    const TradingStatsConfig& config,
    const std::deque<ExtendedTradeRecord>& trades,
    RWSpinLock& trades_mutex
) {
    try {
        namespace fs = std::filesystem;
        fs::create_directories(config.data_dir);

        std::string path = config.data_dir + "/" + config.daily_stats_file;
        std::ofstream file(path);
        if (!file.is_open()) {
            return false;
        }

        // 헤더
        file << "date,trade_count,win_count,loss_count,"
             << "net_pnl_krw,total_volume_krw,"
             << "win_rate,max_drawdown\n";

        auto summaries = get_daily_summaries(365, trades, trades_mutex);

        for (const auto& summary : summaries) {
            double win_rate = (summary.trade_count > 0)
                ? static_cast<double>(summary.win_count) / summary.trade_count * 100.0
                : 0.0;

            file << summary.year << "-"
                 << std::setfill('0') << std::setw(2) << summary.month << "-"
                 << std::setfill('0') << std::setw(2) << summary.day << ","
                 << summary.trade_count << ","
                 << summary.win_count << ","
                 << summary.loss_count << ","
                 << std::fixed << std::setprecision(2)
                 << summary.net_pnl_krw << ","
                 << summary.total_volume_krw << ","
                 << std::setprecision(2) << win_rate << ","
                 << summary.max_drawdown_krw << "\n";
        }

        return true;
    } catch (...) {
        return false;
    }
}

// =============================================================================
// 파일 로드
// =============================================================================

bool StatsReporter::load_trades(
    const TradingStatsConfig& config,
    std::deque<ExtendedTradeRecord>& trades,
    size_t& total_trades_ever,
    RWSpinLock& trades_mutex,
    TradingStats& all_time_stats,
    RWSpinLock& stats_mutex,
    const StatsCalculator& calculator
) {
    try {
        std::string path = config.data_dir + "/" + config.trades_file;
        std::ifstream file(path);
        if (!file.is_open()) {
            return false;  // 파일 없음은 에러 아님
        }

        WriteGuard lock(trades_mutex);
        trades.clear();

        std::string line;
        bool header = true;

        while (std::getline(file, line)) {
            if (header) {
                header = false;
                continue;
            }

            auto fields = parse_csv_line(line);
            if (fields.size() < 14) continue;

            ExtendedTradeRecord record;

            // 타임스탬프 파싱
            std::tm tm = {};
            std::istringstream ss(fields[0]);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            record.timestamp = std::chrono::system_clock::from_time_t(std::mktime(&tm));

            record.trade_id = fields[1];
            record.buy_exchange = fields[2];
            record.sell_exchange = fields[3];
            record.symbol = fields[4];
            record.buy_price = std::stod(fields[5]);
            record.sell_price = std::stod(fields[6]);
            record.quantity = std::stod(fields[7]);
            record.buy_fee_krw = std::stod(fields[8]);
            record.sell_fee_krw = std::stod(fields[9]);
            record.transfer_fee_krw = std::stod(fields[10]);
            record.gross_pnl_krw = std::stod(fields[11]);
            record.net_pnl_krw = std::stod(fields[12]);
            record.premium_pct = std::stod(fields[13]);

            if (fields.size() > 14) record.strategy_name = fields[14];
            if (fields.size() > 15) record.notes = fields[15];

            trades.push_back(record);
            total_trades_ever++;
        }

        // 통계 재계산
        if (!trades.empty()) {
            WriteGuard stats_lock(stats_mutex);
            all_time_stats = calculator.calculate_stats(
                std::vector<ExtendedTradeRecord>(trades.begin(), trades.end()),
                StatsPeriod::AllTime
            );
        }

        return true;
    } catch (...) {
        return false;
    }
}

// =============================================================================
// 집계 포맷팅
// =============================================================================

std::vector<DailySummary> StatsReporter::get_daily_summaries(
    int days,
    const std::deque<ExtendedTradeRecord>& trades,
    RWSpinLock& trades_mutex
) const {
    ReadGuard lock(trades_mutex);

    auto now = std::chrono::system_clock::now();
    auto start = now - std::chrono::hours(24 * days);

    std::vector<DailySummary> summaries;
    std::map<std::tuple<int, int, int>, DailySummary> day_map;

    for (const auto& trade : trades) {
        if (trade.timestamp < start) continue;

        auto t = std::chrono::system_clock::to_time_t(trade.timestamp);
        std::tm tm = *std::localtime(&t);

        auto key = std::make_tuple(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

        auto& summary = day_map[key];
        summary.year = std::get<0>(key);
        summary.month = std::get<1>(key);
        summary.day = std::get<2>(key);

        summary.trade_count++;
        summary.net_pnl_krw += trade.net_pnl_krw;
        summary.total_volume_krw += trade.volume_krw();

        if (trade.net_pnl_krw > 0) {
            summary.win_count++;
        } else if (trade.net_pnl_krw < 0) {
            summary.loss_count++;
        }
    }

    for (auto& [key, summary] : day_map) {
        summaries.push_back(summary);
    }

    // 날짜순 정렬
    std::sort(summaries.begin(), summaries.end(), [](const DailySummary& a, const DailySummary& b) {
        if (a.year != b.year) return a.year < b.year;
        if (a.month != b.month) return a.month < b.month;
        return a.day < b.day;
    });

    return summaries;
}

std::vector<DailySummary> StatsReporter::get_monthly_summaries(
    int months,
    const std::deque<ExtendedTradeRecord>& trades,
    RWSpinLock& trades_mutex
) const {
    ReadGuard lock(trades_mutex);

    auto now = std::chrono::system_clock::now();
    auto start = now - std::chrono::hours(24 * 30 * months);

    std::vector<DailySummary> summaries;
    std::map<std::pair<int, int>, DailySummary> month_map;

    for (const auto& trade : trades) {
        if (trade.timestamp < start) continue;

        auto t = std::chrono::system_clock::to_time_t(trade.timestamp);
        std::tm tm = *std::localtime(&t);

        auto key = std::make_pair(tm.tm_year + 1900, tm.tm_mon + 1);

        auto& summary = month_map[key];
        summary.year = key.first;
        summary.month = key.second;
        summary.day = 0;  // 월간 요약이므로

        summary.trade_count++;
        summary.net_pnl_krw += trade.net_pnl_krw;
        summary.total_volume_krw += trade.volume_krw();

        if (trade.net_pnl_krw > 0) {
            summary.win_count++;
        } else if (trade.net_pnl_krw < 0) {
            summary.loss_count++;
        }
    }

    for (auto& [key, summary] : month_map) {
        summaries.push_back(summary);
    }

    // 월순 정렬
    std::sort(summaries.begin(), summaries.end(), [](const DailySummary& a, const DailySummary& b) {
        if (a.year != b.year) return a.year < b.year;
        return a.month < b.month;
    });

    return summaries;
}

// =============================================================================
// CSV 파싱 헬퍼
// =============================================================================

std::vector<std::string> StatsReporter::parse_csv_line(const std::string& line) const {
    std::vector<std::string> result;
    std::string field;
    bool in_quotes = false;

    for (char c : line) {
        if (c == '"') {
            in_quotes = !in_quotes;
        } else if (c == ',' && !in_quotes) {
            result.push_back(field);
            field.clear();
        } else {
            field += c;
        }
    }
    result.push_back(field);

    return result;
}

}  // namespace arbitrage
