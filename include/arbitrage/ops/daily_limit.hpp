#pragma once

/**
 * Daily Loss Limit (TASK_25)
 *
 * 일일 손실 한도 관리 및 킬스위치
 * - 손익 추적
 * - 한도 체크
 * - 자정 리셋 (KST)
 * - EventBus 연동
 */

#include "arbitrage/common/error.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace arbitrage {

// Forward declarations
class EventBus;

// =============================================================================
// 통계 구조체
// =============================================================================

/**
 * 일일 거래 통계
 */
struct DailyStats {
    double realized_pnl{0.0};       // 실현 손익 (KRW)
    double unrealized_pnl{0.0};     // 미실현 손익 (KRW)
    double total_pnl{0.0};          // 총 손익 (realized + unrealized)
    double max_drawdown{0.0};       // 최대 손실폭
    double peak_pnl{0.0};           // 최고 손익
    int trade_count{0};             // 거래 횟수
    int win_count{0};               // 수익 거래 횟수
    int loss_count{0};              // 손실 거래 횟수
    double total_volume{0.0};       // 총 거래량 (KRW)
    double avg_trade_pnl{0.0};      // 평균 거래 손익
    double largest_win{0.0};        // 최대 수익 거래
    double largest_loss{0.0};       // 최대 손실 거래
    std::chrono::system_clock::time_point reset_time;  // 마지막 리셋 시간
    std::chrono::system_clock::time_point last_trade;  // 마지막 거래 시간

    DailyStats() : reset_time(std::chrono::system_clock::now()) {}

    /**
     * 승률 (0-100%)
     */
    double win_rate() const {
        if (trade_count == 0) return 0.0;
        return static_cast<double>(win_count) / trade_count * 100.0;
    }

    /**
     * 손익비 (평균 수익 / 평균 손실)
     */
    double profit_factor() const {
        if (loss_count == 0 || win_count == 0) return 0.0;
        double avg_win = (win_count > 0) ? (realized_pnl > 0 ? realized_pnl / win_count : 0) : 0;
        double avg_loss = (loss_count > 0) ? (realized_pnl < 0 ? -realized_pnl / loss_count : 0) : 0;
        if (avg_loss == 0) return 0.0;
        return avg_win / avg_loss;
    }
};

/**
 * 거래 기록
 */
struct TradeRecord {
    std::string trade_id;
    double pnl_krw{0.0};
    double volume_krw{0.0};
    std::chrono::system_clock::time_point timestamp;
    std::string notes;

    TradeRecord() : timestamp(std::chrono::system_clock::now()) {}

    TradeRecord(double pnl, double volume = 0.0)
        : pnl_krw(pnl)
        , volume_krw(volume)
        , timestamp(std::chrono::system_clock::now())
    {}
};

// =============================================================================
// 설정
// =============================================================================

struct DailyLimitConfig {
    double daily_loss_limit_krw{1000000.0};     // 일일 손실 한도 (기본 100만원)
    double warning_threshold_pct{70.0};          // 경고 임계값 (70%)
    double critical_threshold_pct{90.0};         // 위험 임계값 (90%)
    bool auto_reset_at_midnight{true};           // 자정 자동 리셋
    int reset_hour_kst{0};                       // 리셋 시각 (KST, 기본 자정)
    bool enable_kill_switch{true};               // 킬스위치 활성화
    bool track_unrealized{false};                // 미실현 손익 포함
    size_t max_trade_history{1000};              // 최대 거래 기록 수
};

// =============================================================================
// 일일 손실 한도 관리자
// =============================================================================

class DailyLossLimiter {
public:
    using KillSwitchCallback = std::function<void()>;
    using WarningCallback = std::function<void(double current_loss, double limit, double pct)>;

    /**
     * 싱글톤 인스턴스
     */
    static DailyLossLimiter& instance();

    DailyLossLimiter();
    explicit DailyLossLimiter(const DailyLimitConfig& config);
    DailyLossLimiter(double limit_krw, KillSwitchCallback kill_switch);
    ~DailyLossLimiter();

    DailyLossLimiter(const DailyLossLimiter&) = delete;
    DailyLossLimiter& operator=(const DailyLossLimiter&) = delete;

    // =========================================================================
    // 서비스 제어
    // =========================================================================

    /**
     * 자동 리셋 타이머 시작
     */
    void start();

    /**
     * 서비스 중지
     */
    void stop();

    /**
     * 실행 중인지 확인
     */
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // =========================================================================
    // 손익 기록
    // =========================================================================

    /**
     * 거래 손익 기록
     * @param pnl_krw 손익 (양수: 수익, 음수: 손실)
     * @param volume_krw 거래량 (선택)
     * @return true if recorded, false if limit reached
     */
    bool record_trade(double pnl_krw, double volume_krw = 0.0);

    /**
     * 거래 기록 (상세)
     */
    bool record_trade(const TradeRecord& record);

    /**
     * 미실현 손익 업데이트
     */
    void update_unrealized_pnl(double unrealized_krw);

    // =========================================================================
    // 상태 조회
    // =========================================================================

    /**
     * 일일 통계 조회
     */
    DailyStats get_stats() const;

    /**
     * 남은 한도
     */
    double remaining_limit() const;

    /**
     * 한도 사용률 (0-100%)
     */
    double usage_percent() const;

    /**
     * 한도 도달 여부
     */
    bool is_limit_reached() const;

    /**
     * 거래 가능 여부
     */
    bool can_trade() const;

    /**
     * 경고 상태 (warning threshold 초과)
     */
    bool is_warning() const;

    /**
     * 위험 상태 (critical threshold 초과)
     */
    bool is_critical() const;

    // =========================================================================
    // 리셋
    // =========================================================================

    /**
     * 수동 리셋
     */
    void reset();

    /**
     * 다음 리셋 시간 조회
     */
    std::chrono::system_clock::time_point next_reset_time() const;

    // =========================================================================
    // 설정
    // =========================================================================

    /**
     * 설정 업데이트
     */
    void set_config(const DailyLimitConfig& config);

    /**
     * 현재 설정
     */
    DailyLimitConfig config() const;

    /**
     * 한도 설정
     */
    void set_limit(double limit_krw);

    /**
     * 킬스위치 콜백 설정
     */
    void set_kill_switch(KillSwitchCallback callback);

    /**
     * 경고 콜백 설정
     */
    void on_warning(WarningCallback callback);

    /**
     * 위험 콜백 설정
     */
    void on_critical(WarningCallback callback);

    /**
     * EventBus 연결
     */
    void set_event_bus(std::shared_ptr<EventBus> bus);

    // =========================================================================
    // 거래 기록
    // =========================================================================

    /**
     * 거래 기록 조회
     */
    std::vector<TradeRecord> get_trade_history() const;

    /**
     * 거래 기록 개수
     */
    size_t trade_history_size() const;

private:
    // =========================================================================
    // 내부 구현
    // =========================================================================

    /**
     * 한도 체크 및 킬스위치 트리거
     */
    void check_and_trigger();

    /**
     * 리셋 타이머 스레드
     */
    void reset_timer_thread();

    /**
     * 다음 자정 시간 계산 (KST)
     */
    std::chrono::system_clock::time_point calculate_next_reset_time() const;

    /**
     * 통계 업데이트
     */
    void update_stats(double pnl_krw, double volume_krw);

private:
    DailyLimitConfig config_;

    // 통계
    mutable std::mutex stats_mutex_;
    DailyStats stats_;
    std::vector<TradeRecord> trade_history_;

    // 상태
    std::atomic<bool> limit_reached_{false};
    std::atomic<bool> warning_triggered_{false};
    std::atomic<bool> critical_triggered_{false};

    // 콜백
    std::mutex callbacks_mutex_;
    KillSwitchCallback kill_switch_callback_;
    WarningCallback warning_callback_;
    WarningCallback critical_callback_;

    // 리셋 타이머
    std::atomic<bool> running_{false};
    std::thread reset_thread_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    // EventBus
    std::weak_ptr<EventBus> event_bus_;
};

// =============================================================================
// 글로벌 접근자
// =============================================================================

/**
 * DailyLossLimiter 싱글톤 접근
 */
inline DailyLossLimiter& daily_limiter() {
    return DailyLossLimiter::instance();
}

// =============================================================================
// 편의 함수
// =============================================================================

/**
 * 거래 기록 (글로벌)
 */
inline bool record_daily_trade(double pnl_krw, double volume_krw = 0.0) {
    return daily_limiter().record_trade(pnl_krw, volume_krw);
}

/**
 * 거래 가능 여부 (글로벌)
 */
inline bool can_trade_today() {
    return daily_limiter().can_trade();
}

/**
 * 남은 한도 (글로벌)
 */
inline double remaining_daily_limit() {
    return daily_limiter().remaining_limit();
}

}  // namespace arbitrage
