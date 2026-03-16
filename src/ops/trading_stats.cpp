/**
 * Trading Stats Implementation (TASK_28)
 */

#include "arbitrage/ops/trading_stats.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace arbitrage {

// =============================================================================
// 싱글톤
// =============================================================================

TradingStatsTracker& TradingStatsTracker::instance() {
    static TradingStatsTracker instance;
    return instance;
}

// =============================================================================
// 생성자/소멸자
// =============================================================================

TradingStatsTracker::TradingStatsTracker() {
    current_equity_.store(config_.initial_capital_krw, std::memory_order_release);
    peak_equity_ = config_.initial_capital_krw;
    all_time_stats_.peak_equity_krw = config_.initial_capital_krw;
    all_time_stats_.start_time = std::chrono::system_clock::now();
}

TradingStatsTracker::TradingStatsTracker(const TradingStatsConfig& config)
    : config_(config)
{
    current_equity_.store(config_.initial_capital_krw, std::memory_order_release);
    peak_equity_ = config_.initial_capital_krw;
    all_time_stats_.peak_equity_krw = config_.initial_capital_krw;
    all_time_stats_.start_time = std::chrono::system_clock::now();
}

TradingStatsTracker::~TradingStatsTracker() {
    stop();
    if (config_.auto_save) {
        save();
    }
}

// =============================================================================
// 서비스 제어
// =============================================================================

void TradingStatsTracker::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // 이미 실행 중
    }

    // 데이터 로드
    load();

    // 자동 저장 스레드 시작
    if (config_.auto_save) {
        auto_save_thread_ = std::thread(&TradingStatsTracker::auto_save_thread, this);
    }
}

void TradingStatsTracker::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;  // 이미 중지됨
    }

    // 조건 변수 깨우기
    {
        std::lock_guard<std::mutex> lock(cv_mutex_);
        cv_.notify_all();
    }

    // 스레드 정리
    if (auto_save_thread_.joinable()) {
        auto_save_thread_.join();
    }
}

// =============================================================================
// 거래 기록
// =============================================================================

void TradingStatsTracker::record_trade(double pnl_krw, double volume_krw) {
    ExtendedTradeRecord record;
    record.net_pnl_krw = pnl_krw;
    record.gross_pnl_krw = pnl_krw;  // 수수료 정보 없음
    record.timestamp = std::chrono::system_clock::now();

    // 거래량을 수량으로 변환 (가격 정보 없을 때)
    if (volume_krw > 0) {
        record.buy_price = volume_krw / 2.0;  // 대략적 분배
        record.sell_price = volume_krw / 2.0;
        record.quantity = 1.0;
    }

    record_trade(record);
}

void TradingStatsTracker::record_trade(const TradeRecord& record) {
    ExtendedTradeRecord ext;
    ext.trade_id = record.trade_id;
    ext.net_pnl_krw = record.pnl_krw;
    ext.gross_pnl_krw = record.pnl_krw;
    ext.timestamp = record.timestamp;
    ext.notes = record.notes;

    if (record.volume_krw > 0) {
        ext.buy_price = record.volume_krw / 2.0;
        ext.sell_price = record.volume_krw / 2.0;
        ext.quantity = 1.0;
    }

    record_trade(ext);
}

void TradingStatsTracker::record_trade(const ExtendedTradeRecord& record) {
    std::lock_guard<std::mutex> lock(trades_mutex_);

    // 거래 추가
    trades_.push_back(record);
    total_trades_ever_++;

    // 메모리 제한
    while (trades_.size() > config_.max_trades_in_memory) {
        trades_.pop_front();
    }

    // 전체 통계 업데이트
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);

        all_time_stats_.total_trades++;
        all_time_stats_.end_time = record.timestamp;

        double pnl = record.net_pnl_krw;
        all_time_stats_.net_pnl_krw += pnl;
        all_time_stats_.total_volume_krw += record.volume_krw();

        if (pnl > 0) {
            all_time_stats_.winning_trades++;
            all_time_stats_.total_profit_krw += pnl;
            if (pnl > all_time_stats_.largest_win_krw) {
                all_time_stats_.largest_win_krw = pnl;
            }
        } else if (pnl < 0) {
            all_time_stats_.losing_trades++;
            all_time_stats_.total_loss_krw += std::abs(pnl);
            if (std::abs(pnl) > all_time_stats_.largest_loss_krw) {
                all_time_stats_.largest_loss_krw = std::abs(pnl);
            }
        } else {
            all_time_stats_.break_even_trades++;
        }

        // 평균 계산
        if (all_time_stats_.winning_trades > 0) {
            all_time_stats_.avg_win_krw =
                all_time_stats_.total_profit_krw / all_time_stats_.winning_trades;
        }
        if (all_time_stats_.losing_trades > 0) {
            all_time_stats_.avg_loss_krw =
                all_time_stats_.total_loss_krw / all_time_stats_.losing_trades;
        }

        // 드로다운 업데이트
        update_drawdown(pnl);

        // 연속 기록 업데이트
        update_streak(pnl > 0);
    }
}

void TradingStatsTracker::update_equity(double equity_krw) {
    current_equity_.store(equity_krw, std::memory_order_release);

    std::lock_guard<std::mutex> lock(stats_mutex_);

    if (equity_krw > peak_equity_) {
        peak_equity_ = equity_krw;
        all_time_stats_.peak_equity_krw = equity_krw;
    }

    // 드로다운 계산
    if (peak_equity_ > 0) {
        double dd_krw = peak_equity_ - equity_krw;
        double dd_pct = dd_krw / peak_equity_ * 100.0;

        if (dd_krw > max_drawdown_krw_) {
            max_drawdown_krw_ = dd_krw;
            all_time_stats_.max_drawdown_krw = dd_krw;
        }
        if (dd_pct > max_drawdown_pct_) {
            max_drawdown_pct_ = dd_pct;
            all_time_stats_.max_drawdown_pct = dd_pct;
        }
    }
}

// =============================================================================
// 통계 조회
// =============================================================================

TradingStats TradingStatsTracker::get_daily_stats() const {
    return get_stats(StatsPeriod::Daily);
}

TradingStats TradingStatsTracker::get_weekly_stats() const {
    return get_stats(StatsPeriod::Weekly);
}

TradingStats TradingStatsTracker::get_monthly_stats() const {
    return get_stats(StatsPeriod::Monthly);
}

TradingStats TradingStatsTracker::get_yearly_stats() const {
    return get_stats(StatsPeriod::Yearly);
}

TradingStats TradingStatsTracker::get_all_time_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return all_time_stats_;
}

TradingStats TradingStatsTracker::get_stats(StatsPeriod period) const {
    auto now = std::chrono::system_clock::now();
    auto start = now;

    // 기간 시작점 계산
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now = *std::localtime(&now_t);

    switch (period) {
        case StatsPeriod::Daily:
            tm_now.tm_hour = 0;
            tm_now.tm_min = 0;
            tm_now.tm_sec = 0;
            start = std::chrono::system_clock::from_time_t(std::mktime(&tm_now));
            break;

        case StatsPeriod::Weekly:
            // 이번 주 월요일
            tm_now.tm_hour = 0;
            tm_now.tm_min = 0;
            tm_now.tm_sec = 0;
            tm_now.tm_mday -= (tm_now.tm_wday == 0) ? 6 : (tm_now.tm_wday - 1);
            start = std::chrono::system_clock::from_time_t(std::mktime(&tm_now));
            break;

        case StatsPeriod::Monthly:
            tm_now.tm_hour = 0;
            tm_now.tm_min = 0;
            tm_now.tm_sec = 0;
            tm_now.tm_mday = 1;
            start = std::chrono::system_clock::from_time_t(std::mktime(&tm_now));
            break;

        case StatsPeriod::Yearly:
            tm_now.tm_hour = 0;
            tm_now.tm_min = 0;
            tm_now.tm_sec = 0;
            tm_now.tm_mday = 1;
            tm_now.tm_mon = 0;
            start = std::chrono::system_clock::from_time_t(std::mktime(&tm_now));
            break;

        case StatsPeriod::AllTime:
            return get_all_time_stats();
    }

    return get_stats(start, now);
}

TradingStats TradingStatsTracker::get_stats(
    std::chrono::system_clock::time_point start,
    std::chrono::system_clock::time_point end
) const {
    std::lock_guard<std::mutex> lock(trades_mutex_);

    std::vector<ExtendedTradeRecord> filtered;
    for (const auto& trade : trades_) {
        if (trade.timestamp >= start && trade.timestamp <= end) {
            filtered.push_back(trade);
        }
    }

    return calculate_stats(filtered, StatsPeriod::AllTime);
}

// =============================================================================
// 일별 요약
// =============================================================================

std::vector<DailySummary> TradingStatsTracker::get_daily_summaries(int days) const {
    std::lock_guard<std::mutex> lock(trades_mutex_);

    auto now = std::chrono::system_clock::now();
    auto start = now - std::chrono::hours(24 * days);

    std::vector<DailySummary> summaries;
    std::map<std::tuple<int, int, int>, DailySummary> day_map;

    for (const auto& trade : trades_) {
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

std::vector<DailySummary> TradingStatsTracker::get_monthly_summaries(int months) const {
    std::lock_guard<std::mutex> lock(trades_mutex_);

    auto now = std::chrono::system_clock::now();
    auto start = now - std::chrono::hours(24 * 30 * months);

    std::vector<DailySummary> summaries;
    std::map<std::pair<int, int>, DailySummary> month_map;

    for (const auto& trade : trades_) {
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
// 거래 기록 조회
// =============================================================================

size_t TradingStatsTracker::total_trade_count() const {
    return total_trades_ever_;
}

std::vector<ExtendedTradeRecord> TradingStatsTracker::get_recent_trades(size_t count) const {
    std::lock_guard<std::mutex> lock(trades_mutex_);

    std::vector<ExtendedTradeRecord> result;
    size_t start = (trades_.size() > count) ? (trades_.size() - count) : 0;

    for (size_t i = start; i < trades_.size(); ++i) {
        result.push_back(trades_[i]);
    }

    return result;
}

std::vector<ExtendedTradeRecord> TradingStatsTracker::get_trades(
    std::chrono::system_clock::time_point start,
    std::chrono::system_clock::time_point end
) const {
    std::lock_guard<std::mutex> lock(trades_mutex_);

    std::vector<ExtendedTradeRecord> result;
    for (const auto& trade : trades_) {
        if (trade.timestamp >= start && trade.timestamp <= end) {
            result.push_back(trade);
        }
    }

    return result;
}

// =============================================================================
// 파일 저장/로드
// =============================================================================

bool TradingStatsTracker::save() {
    return save_trades() && save_daily_stats();
}

bool TradingStatsTracker::save_trades() {
    try {
        namespace fs = std::filesystem;
        fs::create_directories(config_.data_dir);

        std::string path = config_.data_dir + "/" + config_.trades_file;
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

        std::lock_guard<std::mutex> lock(trades_mutex_);

        for (const auto& trade : trades_) {
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

bool TradingStatsTracker::save_daily_stats() {
    try {
        namespace fs = std::filesystem;
        fs::create_directories(config_.data_dir);

        std::string path = config_.data_dir + "/" + config_.daily_stats_file;
        std::ofstream file(path);
        if (!file.is_open()) {
            return false;
        }

        // 헤더
        file << "date,trade_count,win_count,loss_count,"
             << "net_pnl_krw,total_volume_krw,"
             << "win_rate,max_drawdown\n";

        auto summaries = get_daily_summaries(365);  // 최근 1년

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

bool TradingStatsTracker::load() {
    return load_trades();
}

bool TradingStatsTracker::load_trades() {
    try {
        std::string path = config_.data_dir + "/" + config_.trades_file;
        std::ifstream file(path);
        if (!file.is_open()) {
            return false;  // 파일 없음은 에러 아님
        }

        std::lock_guard<std::mutex> lock(trades_mutex_);
        trades_.clear();

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

            trades_.push_back(record);
            total_trades_ever_++;
        }

        // 통계 재계산
        if (!trades_.empty()) {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            all_time_stats_ = calculate_stats(
                std::vector<ExtendedTradeRecord>(trades_.begin(), trades_.end()),
                StatsPeriod::AllTime
            );
        }

        return true;
    } catch (...) {
        return false;
    }
}

// =============================================================================
// 설정
// =============================================================================

void TradingStatsTracker::set_config(const TradingStatsConfig& config) {
    config_ = config;
}

TradingStatsConfig TradingStatsTracker::config() const {
    return config_;
}

void TradingStatsTracker::on_daily_close(StatsCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    daily_close_callback_ = std::move(callback);
}

// =============================================================================
// 리셋
// =============================================================================

void TradingStatsTracker::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    all_time_stats_ = TradingStats();
    all_time_stats_.start_time = std::chrono::system_clock::now();
    all_time_stats_.peak_equity_krw = current_equity_.load(std::memory_order_acquire);

    peak_equity_ = current_equity_.load(std::memory_order_acquire);
    max_drawdown_krw_ = 0.0;
    max_drawdown_pct_ = 0.0;
    current_streak_ = 0;
    max_consecutive_wins_ = 0;
    max_consecutive_losses_ = 0;
}

void TradingStatsTracker::reset_all() {
    reset_stats();

    {
        std::lock_guard<std::mutex> lock(trades_mutex_);
        trades_.clear();
        total_trades_ever_ = 0;
    }

    daily_summaries_.clear();
}

// =============================================================================
// 내부 구현
// =============================================================================

TradingStats TradingStatsTracker::calculate_stats(
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

void TradingStatsTracker::update_drawdown(double pnl_krw) {
    // current_equity는 외부에서 업데이트됨
    // 여기서는 통계만 업데이트
    all_time_stats_.max_drawdown_krw = max_drawdown_krw_;
    all_time_stats_.max_drawdown_pct = max_drawdown_pct_;
}

void TradingStatsTracker::update_streak(bool is_win) {
    if (is_win) {
        if (current_streak_ >= 0) {
            current_streak_++;
        } else {
            current_streak_ = 1;
        }
        if (current_streak_ > max_consecutive_wins_) {
            max_consecutive_wins_ = current_streak_;
        }
    } else {
        if (current_streak_ <= 0) {
            current_streak_--;
        } else {
            current_streak_ = -1;
        }
        if ((-current_streak_) > max_consecutive_losses_) {
            max_consecutive_losses_ = -current_streak_;
        }
    }

    all_time_stats_.current_streak = current_streak_;
    all_time_stats_.max_consecutive_wins = max_consecutive_wins_;
    all_time_stats_.max_consecutive_losses = max_consecutive_losses_;
}

void TradingStatsTracker::auto_save_thread() {
    while (running_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::seconds(config_.auto_save_interval_sec), [this] {
            return !running_.load(std::memory_order_acquire);
        });

        if (running_.load(std::memory_order_acquire)) {
            save();
        }
    }
}

std::vector<std::string> TradingStatsTracker::parse_csv_line(const std::string& line) const {
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

bool TradingStatsTracker::is_same_day(
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

bool TradingStatsTracker::is_same_week(
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

bool TradingStatsTracker::is_same_month(
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
