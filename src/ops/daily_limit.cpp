#include "arbitrage/ops/daily_limit.hpp"
#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace arbitrage {

// =============================================================================
// 싱글톤
// =============================================================================

namespace { DailyLossLimiter* g_set_daily_limiter_override = nullptr; }
DailyLossLimiter& DailyLossLimiter::instance() {
    if (g_set_daily_limiter_override) return *g_set_daily_limiter_override;
    static DailyLossLimiter instance;
    return instance;
}
void set_daily_limiter(DailyLossLimiter* p) { g_set_daily_limiter_override = p; }

// =============================================================================
// 생성자/소멸자
// =============================================================================

DailyLossLimiter::DailyLossLimiter() {
    stats_.reset_time = std::chrono::system_clock::now();
}

DailyLossLimiter::DailyLossLimiter(const DailyLimitConfig& config)
    : config_(config)
{
    stats_.reset_time = std::chrono::system_clock::now();
}

DailyLossLimiter::DailyLossLimiter(double limit_krw, KillSwitchCallback kill_switch)
    : kill_switch_callback_(std::move(kill_switch))
{
    config_.daily_loss_limit_krw = limit_krw;
    stats_.reset_time = std::chrono::system_clock::now();
}

DailyLossLimiter::~DailyLossLimiter() {
    stop();
}

// =============================================================================
// 서비스 제어
// =============================================================================

void DailyLossLimiter::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;  // 이미 실행 중
    }

    if (config_.auto_reset_at_midnight) {
        reset_thread_ = std::thread(&DailyLossLimiter::reset_timer_thread, this);
    }
}

void DailyLossLimiter::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    wakeup_.store(true, std::memory_order_release);

    if (reset_thread_.joinable()) {
        reset_thread_.join();
    }
}

// =============================================================================
// 손익 기록
// =============================================================================

bool DailyLossLimiter::record_trade(double pnl_krw, double volume_krw) {
    if (limit_reached_.load(std::memory_order_acquire)) {
        return false;
    }

    {
        WriteGuard lock(stats_mutex_);
        update_stats(pnl_krw, volume_krw);

        // 거래 기록 추가
        TradeRecord record(pnl_krw, volume_krw);
        trade_history_.push_back(record);

        // 최대 기록 수 초과 시 오래된 것 삭제
        while (trade_history_.size() > config_.max_trade_history) {
            trade_history_.erase(trade_history_.begin());
        }
    }

    check_and_trigger();
    return !limit_reached_.load(std::memory_order_acquire);
}

bool DailyLossLimiter::record_trade(const TradeRecord& record) {
    if (limit_reached_.load(std::memory_order_acquire)) {
        return false;
    }

    {
        WriteGuard lock(stats_mutex_);
        update_stats(record.pnl_krw, record.volume_krw);

        trade_history_.push_back(record);

        while (trade_history_.size() > config_.max_trade_history) {
            trade_history_.erase(trade_history_.begin());
        }
    }

    check_and_trigger();
    return !limit_reached_.load(std::memory_order_acquire);
}

void DailyLossLimiter::update_unrealized_pnl(double unrealized_krw) {
    WriteGuard lock(stats_mutex_);
    stats_.unrealized_pnl = unrealized_krw;
    stats_.total_pnl = stats_.realized_pnl + unrealized_krw;

    // 드로우다운 업데이트
    if (stats_.total_pnl > stats_.peak_pnl) {
        stats_.peak_pnl = stats_.total_pnl;
    }
    double current_drawdown = stats_.peak_pnl - stats_.total_pnl;
    if (current_drawdown > stats_.max_drawdown) {
        stats_.max_drawdown = current_drawdown;
    }
}

// =============================================================================
// 상태 조회
// =============================================================================

DailyStats DailyLossLimiter::get_stats() const {
    ReadGuard lock(stats_mutex_);
    return stats_;
}

double DailyLossLimiter::remaining_limit() const {
    ReadGuard lock(stats_mutex_);

    double current_loss = -stats_.realized_pnl;
    if (config_.track_unrealized) {
        current_loss = -stats_.total_pnl;
    }

    if (current_loss < 0) {
        // 수익 상태면 전체 한도가 남음
        return config_.daily_loss_limit_krw;
    }

    return std::max(0.0, config_.daily_loss_limit_krw - current_loss);
}

double DailyLossLimiter::usage_percent() const {
    ReadGuard lock(stats_mutex_);

    double current_loss = -stats_.realized_pnl;
    if (config_.track_unrealized) {
        current_loss = -stats_.total_pnl;
    }

    if (current_loss <= 0) {
        return 0.0;  // 손실 없음
    }

    return std::min(100.0, (current_loss / config_.daily_loss_limit_krw) * 100.0);
}

bool DailyLossLimiter::is_limit_reached() const {
    return limit_reached_.load(std::memory_order_acquire);
}

bool DailyLossLimiter::can_trade() const {
    return !limit_reached_.load(std::memory_order_acquire);
}

bool DailyLossLimiter::is_warning() const {
    return warning_triggered_.load(std::memory_order_acquire);
}

bool DailyLossLimiter::is_critical() const {
    return critical_triggered_.load(std::memory_order_acquire);
}

// =============================================================================
// 리셋
// =============================================================================

void DailyLossLimiter::reset() {
    {
        WriteGuard lock(stats_mutex_);
        stats_ = DailyStats();
        stats_.reset_time = std::chrono::system_clock::now();
        trade_history_.clear();
    }

    limit_reached_.store(false, std::memory_order_release);
    warning_triggered_.store(false, std::memory_order_release);
    critical_triggered_.store(false, std::memory_order_release);
}

std::chrono::system_clock::time_point DailyLossLimiter::next_reset_time() const {
    return calculate_next_reset_time();
}

// =============================================================================
// 설정
// =============================================================================

void DailyLossLimiter::set_config(const DailyLimitConfig& config) {
    WriteGuard lock(stats_mutex_);
    config_ = config;
}

DailyLimitConfig DailyLossLimiter::config() const {
    ReadGuard lock(stats_mutex_);
    return config_;
}

void DailyLossLimiter::set_limit(double limit_krw) {
    WriteGuard lock(stats_mutex_);
    config_.daily_loss_limit_krw = limit_krw;
}

void DailyLossLimiter::set_kill_switch(KillSwitchCallback callback) {
    SpinLockGuard lock(callbacks_mutex_);
    kill_switch_callback_ = std::move(callback);
}

void DailyLossLimiter::on_warning(WarningCallback callback) {
    SpinLockGuard lock(callbacks_mutex_);
    warning_callback_ = std::move(callback);
}

void DailyLossLimiter::on_critical(WarningCallback callback) {
    SpinLockGuard lock(callbacks_mutex_);
    critical_callback_ = std::move(callback);
}

void DailyLossLimiter::set_event_bus(std::shared_ptr<EventBus> bus) {
    SpinLockGuard lock(callbacks_mutex_);
    event_bus_ = bus;
}

// =============================================================================
// 거래 기록
// =============================================================================

std::vector<TradeRecord> DailyLossLimiter::get_trade_history() const {
    ReadGuard lock(stats_mutex_);
    return trade_history_;
}

size_t DailyLossLimiter::trade_history_size() const {
    ReadGuard lock(stats_mutex_);
    return trade_history_.size();
}

// =============================================================================
// 내부 구현
// =============================================================================

void DailyLossLimiter::check_and_trigger() {
    double current_loss = 0.0;
    double limit = 0.0;

    {
        ReadGuard lock(stats_mutex_);
        current_loss = -stats_.realized_pnl;
        if (config_.track_unrealized) {
            current_loss = -stats_.total_pnl;
        }
        limit = config_.daily_loss_limit_krw;
    }

    if (current_loss <= 0) {
        return;  // 손실 없음
    }

    double usage_pct = (current_loss / limit) * 100.0;

    // 경고 체크
    if (usage_pct >= config_.warning_threshold_pct &&
        !warning_triggered_.exchange(true, std::memory_order_acq_rel))
    {
        WarningCallback cb;
        {
            SpinLockGuard lock(callbacks_mutex_);
            cb = warning_callback_;
        }
        if (cb) {
            try {
                cb(current_loss, limit, usage_pct);
            } catch (const std::exception& e) {
                // 경고 콜백 에러 (처리 계속)
            }
        }
    }

    // 위험 체크
    if (usage_pct >= config_.critical_threshold_pct &&
        !critical_triggered_.exchange(true, std::memory_order_acq_rel))
    {
        WarningCallback cb;
        {
            SpinLockGuard lock(callbacks_mutex_);
            cb = critical_callback_;
        }
        if (cb) {
            try {
                cb(current_loss, limit, usage_pct);
            } catch (const std::exception& e) {
                // 위험 콜백 에러 (처리 계속)
            }
        }
    }

    // 한도 도달 체크
    if (current_loss >= limit &&
        !limit_reached_.exchange(true, std::memory_order_acq_rel))
    {
        // 킬스위치 활성화
        if (config_.enable_kill_switch) {
            KillSwitchCallback kill_cb;
            {
                SpinLockGuard lock(callbacks_mutex_);
                kill_cb = kill_switch_callback_;
            }
            if (kill_cb) {
                try {
                    kill_cb();
                } catch (const std::exception& e) {
                    // 킬스위치 콜백 에러 (처리 계속)
                }
            }

            // EventBus 이벤트 발행
            auto bus = event_bus_.lock();
            if (bus) {
                events::DailyLossLimitReached event;
                event.loss_amount = current_loss;
                event.limit = limit;
                bus->publish(event);

                events::KillSwitchActivated kill_event;
                kill_event.reason = "Daily loss limit reached";
                kill_event.manual = false;
                bus->publish(kill_event);
            }
        }
    }
}

void DailyLossLimiter::reset_timer_thread() {
    while (running_.load(std::memory_order_acquire)) {
        auto next_reset = calculate_next_reset_time();
        auto now = std::chrono::system_clock::now();

        if (next_reset <= now) {
            // 이미 리셋 시간이 지났으면 다음 날로
            next_reset = next_reset + std::chrono::hours(24);
        }

        auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(next_reset - now);

        wakeup_.store(false, std::memory_order_release);
        bool woken = SpinWait::until_for(
            [this]() {
                return wakeup_.load(std::memory_order_acquire) ||
                       !running_.load(std::memory_order_acquire);
            },
            wait_time
        );

        if (woken && !running_.load(std::memory_order_acquire)) {
            break;  // 종료 요청
        }

        if (running_.load(std::memory_order_acquire)) {
            reset();
        }
    }
}

std::chrono::system_clock::time_point DailyLossLimiter::calculate_next_reset_time() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    // KST = UTC + 9시간
    constexpr int kst_offset_hours = 9;

    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    // 현재 로컬 시간 기준으로 다음 리셋 시간 계산
    // (간단화를 위해 로컬 타임존이 KST라고 가정)
    std::tm tm_reset = tm_now;
    tm_reset.tm_hour = config_.reset_hour_kst;
    tm_reset.tm_min = 0;
    tm_reset.tm_sec = 0;

    auto reset_time_t = std::mktime(&tm_reset);
    auto reset_point = std::chrono::system_clock::from_time_t(reset_time_t);

    // 이미 오늘의 리셋 시간이 지났으면 내일로
    if (reset_point <= now) {
        reset_point += std::chrono::hours(24);
    }

    return reset_point;
}

void DailyLossLimiter::update_stats(double pnl_krw, double volume_krw) {
    // 손익 업데이트
    stats_.realized_pnl += pnl_krw;
    stats_.total_pnl = stats_.realized_pnl + stats_.unrealized_pnl;
    stats_.trade_count++;
    stats_.total_volume += volume_krw;
    stats_.last_trade = std::chrono::system_clock::now();

    // 승/패 카운트
    if (pnl_krw > 0) {
        stats_.win_count++;
        if (pnl_krw > stats_.largest_win) {
            stats_.largest_win = pnl_krw;
        }
    } else if (pnl_krw < 0) {
        stats_.loss_count++;
        if (pnl_krw < stats_.largest_loss) {
            stats_.largest_loss = pnl_krw;
        }
    }

    // 평균 계산
    if (stats_.trade_count > 0) {
        stats_.avg_trade_pnl = stats_.realized_pnl / stats_.trade_count;
    }

    // 피크/드로우다운 업데이트
    if (stats_.total_pnl > stats_.peak_pnl) {
        stats_.peak_pnl = stats_.total_pnl;
    }
    double current_drawdown = stats_.peak_pnl - stats_.total_pnl;
    if (current_drawdown > stats_.max_drawdown) {
        stats_.max_drawdown = current_drawdown;
    }
}

}  // namespace arbitrage
