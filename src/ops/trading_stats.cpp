/**
 * Trading Stats Implementation (TASK_28)
 *
 * Orchestrator — delegates calculation to StatsCalculator
 * and I/O to StatsReporter.
 */

#include "arbitrage/ops/trading_stats.hpp"

#include <algorithm>
#include <ctime>

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

        // 드로다운 업데이트 (delegate to calculator)
        calculator_.update_drawdown(all_time_stats_, max_drawdown_krw_, max_drawdown_pct_);

        // 연속 기록 업데이트 (delegate to calculator)
        calculator_.update_streak(
            pnl > 0,
            current_streak_,
            max_consecutive_wins_,
            max_consecutive_losses_,
            all_time_stats_
        );
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

    return calculator_.calculate_stats(filtered, StatsPeriod::AllTime);
}

// =============================================================================
// 일별 요약 (delegate to reporter)
// =============================================================================

std::vector<DailySummary> TradingStatsTracker::get_daily_summaries(int days) const {
    return reporter_.get_daily_summaries(days, trades_, trades_mutex_);
}

std::vector<DailySummary> TradingStatsTracker::get_monthly_summaries(int months) const {
    return reporter_.get_monthly_summaries(months, trades_, trades_mutex_);
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
// 파일 저장/로드 (delegate to reporter)
// =============================================================================

bool TradingStatsTracker::save() {
    return save_trades() && save_daily_stats();
}

bool TradingStatsTracker::save_trades() {
    return reporter_.save_trades(config_, trades_, trades_mutex_);
}

bool TradingStatsTracker::save_daily_stats() {
    return reporter_.save_daily_stats(config_, trades_, trades_mutex_);
}

bool TradingStatsTracker::load() {
    return load_trades();
}

bool TradingStatsTracker::load_trades() {
    return reporter_.load_trades(
        config_, trades_, total_trades_ever_,
        trades_mutex_, all_time_stats_, stats_mutex_,
        calculator_
    );
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

}  // namespace arbitrage
