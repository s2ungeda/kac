#pragma once

/**
 * Trading Stats (TASK_28)
 *
 * 거래 실적 통계 및 리포팅
 * - 일/주/월/전체 통계
 * - 승률, 손익비, 샤프 비율
 * - 드로다운 추적
 * - 파일 기반 저장
 *
 * Delegates calculation to StatsCalculator and I/O to StatsReporter.
 */

#include "arbitrage/ops/daily_limit.hpp"
#include "arbitrage/ops/stats_calculator.hpp"
#include "arbitrage/ops/stats_reporter.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace arbitrage {

// =============================================================================
// 기간 타입
// =============================================================================

enum class StatsPeriod {
    Daily,      // 일간
    Weekly,     // 주간
    Monthly,    // 월간
    Yearly,     // 연간
    AllTime     // 전체
};

// =============================================================================
// 거래 통계 구조체 (확장)
// =============================================================================

/**
 * 거래 통계 (기간별)
 */
struct TradingStats {
    // 기간
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    StatsPeriod period{StatsPeriod::AllTime};

    // 거래 횟수
    int total_trades{0};
    int winning_trades{0};
    int losing_trades{0};
    int break_even_trades{0};  // 손익 0인 거래

    // 손익 (KRW)
    double total_profit_krw{0.0};    // 총 수익
    double total_loss_krw{0.0};      // 총 손실 (절대값)
    double net_pnl_krw{0.0};         // 순 손익

    // 거래량
    double total_volume_krw{0.0};    // 총 거래량

    // 드로다운
    double max_drawdown_krw{0.0};    // 최대 손실폭 (절대값)
    double max_drawdown_pct{0.0};    // 최대 손실폭 (퍼센트)
    double peak_equity_krw{0.0};     // 최고 자산

    // 연속 기록
    int max_consecutive_wins{0};
    int max_consecutive_losses{0};
    int current_streak{0};  // 양수: 연승, 음수: 연패

    // 최대/최소 거래
    double largest_win_krw{0.0};
    double largest_loss_krw{0.0};
    double avg_win_krw{0.0};
    double avg_loss_krw{0.0};

    // 리스크/리워드 분석
    std::vector<double> daily_returns;  // 일별 수익률 (샤프 비율용)

    TradingStats()
        : start_time(std::chrono::system_clock::now())
        , end_time(std::chrono::system_clock::now())
    {}

    // =========================================================================
    // 분석 함수
    // =========================================================================

    /**
     * 승률 (0-100%)
     */
    double win_rate() const {
        if (total_trades == 0) return 0.0;
        return static_cast<double>(winning_trades) / total_trades * 100.0;
    }

    /**
     * 손익비 (Profit Factor)
     * 총 수익 / 총 손실
     */
    double profit_factor() const {
        if (total_loss_krw == 0.0) {
            return (total_profit_krw > 0) ? 999.99 : 0.0;
        }
        return total_profit_krw / total_loss_krw;
    }

    /**
     * 평균 수익 (승리 거래)
     */
    double avg_profit() const {
        if (winning_trades == 0) return 0.0;
        return total_profit_krw / winning_trades;
    }

    /**
     * 평균 손실 (손실 거래, 절대값)
     */
    double avg_loss() const {
        if (losing_trades == 0) return 0.0;
        return total_loss_krw / losing_trades;
    }

    /**
     * 기대값 (거래당 평균 손익)
     */
    double expectancy() const {
        if (total_trades == 0) return 0.0;
        return net_pnl_krw / total_trades;
    }

    /**
     * 리스크/리워드 비율
     * 평균 수익 / 평균 손실
     */
    double risk_reward_ratio() const {
        double avg_l = avg_loss();
        if (avg_l == 0.0) return 0.0;
        return avg_profit() / avg_l;
    }

    /**
     * 샤프 비율 (연환산)
     * (평균 수익률 - 무위험 수익률) / 표준편차
     * 무위험 수익률: 연 3% 가정 → 일 0.0082%
     */
    double sharpe_ratio(double risk_free_daily = 0.000082) const {
        if (daily_returns.empty()) return 0.0;

        // 평균 일별 수익률
        double sum = 0.0;
        for (double r : daily_returns) {
            sum += r;
        }
        double mean = sum / daily_returns.size();

        // 표준편차
        double variance = 0.0;
        for (double r : daily_returns) {
            variance += (r - mean) * (r - mean);
        }
        variance /= daily_returns.size();
        double std_dev = std::sqrt(variance);

        if (std_dev == 0.0) return 0.0;

        // 연환산 (√252 거래일)
        return (mean - risk_free_daily) / std_dev * std::sqrt(252);
    }

    /**
     * 최대 드로다운 (%)
     */
    double max_drawdown() const {
        return max_drawdown_pct;
    }

    /**
     * Calmar Ratio (연수익률 / 최대 드로다운)
     */
    double calmar_ratio() const {
        if (max_drawdown_pct == 0.0) return 0.0;

        // 기간(일) 계산
        auto duration = std::chrono::duration_cast<std::chrono::hours>(end_time - start_time);
        double days = duration.count() / 24.0;
        if (days < 1.0) days = 1.0;

        // 연환산 수익률
        double annual_return = (net_pnl_krw / peak_equity_krw) * (365.0 / days);
        return annual_return / max_drawdown_pct * 100.0;
    }

    /**
     * 회복 계수 (순수익 / 최대 드로다운)
     */
    double recovery_factor() const {
        if (max_drawdown_krw == 0.0) return 0.0;
        return net_pnl_krw / max_drawdown_krw;
    }
};

// =============================================================================
// 설정
// =============================================================================

struct TradingStatsConfig {
    std::string data_dir{"data/stats"};          // 데이터 저장 디렉토리
    std::string trades_file{"trades.csv"};        // 거래 기록 파일
    std::string daily_stats_file{"daily.csv"};    // 일별 통계 파일
    size_t max_trades_in_memory{10000};           // 메모리 최대 거래 수
    bool auto_save{true};                         // 자동 저장
    int auto_save_interval_sec{300};              // 자동 저장 주기 (5분)
    double initial_capital_krw{10000000.0};       // 초기 자본 (1000만원)
};

// =============================================================================
// 거래 기록 (확장)
// =============================================================================

/**
 * 거래 기록 (상세)
 */
struct ExtendedTradeRecord {
    // 기본 정보
    std::string trade_id;
    std::chrono::system_clock::time_point timestamp;

    // 거래소 정보
    std::string buy_exchange;
    std::string sell_exchange;
    std::string symbol{"XRP"};

    // 가격 정보
    double buy_price{0.0};
    double sell_price{0.0};
    double quantity{0.0};

    // 비용
    double buy_fee_krw{0.0};
    double sell_fee_krw{0.0};
    double transfer_fee_krw{0.0};

    // 손익
    double gross_pnl_krw{0.0};    // 총 손익 (수수료 전)
    double net_pnl_krw{0.0};      // 순 손익 (수수료 후)
    double premium_pct{0.0};      // 프리미엄 (%)

    // 메타데이터
    std::string strategy_name;
    std::string notes;

    ExtendedTradeRecord() : timestamp(std::chrono::system_clock::now()) {}

    /**
     * 총 거래량 (KRW)
     */
    double volume_krw() const {
        return buy_price * quantity + sell_price * quantity;
    }

    /**
     * 총 수수료
     */
    double total_fees() const {
        return buy_fee_krw + sell_fee_krw + transfer_fee_krw;
    }
};

// =============================================================================
// 일별 요약
// =============================================================================

struct DailySummary {
    int year{0};
    int month{0};
    int day{0};

    int trade_count{0};
    int win_count{0};
    int loss_count{0};

    double net_pnl_krw{0.0};
    double total_volume_krw{0.0};
    double max_drawdown_krw{0.0};

    double equity_start{0.0};   // 시작 자산
    double equity_end{0.0};     // 종료 자산

    double return_pct() const {
        if (equity_start == 0.0) return 0.0;
        return (equity_end - equity_start) / equity_start * 100.0;
    }
};

// =============================================================================
// Trading Stats Tracker
// =============================================================================

class TradingStatsTracker {
public:
    using StatsCallback = std::function<void(const TradingStats&)>;

    /**
     * 싱글톤 인스턴스
     */
    static TradingStatsTracker& instance();

    TradingStatsTracker();
    explicit TradingStatsTracker(const TradingStatsConfig& config);
    ~TradingStatsTracker();

    TradingStatsTracker(const TradingStatsTracker&) = delete;
    TradingStatsTracker& operator=(const TradingStatsTracker&) = delete;

    // =========================================================================
    // 서비스 제어
    // =========================================================================

    /**
     * 서비스 시작 (자동 저장 타이머)
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
    // 거래 기록
    // =========================================================================

    /**
     * 거래 기록 (간단)
     */
    void record_trade(double pnl_krw, double volume_krw = 0.0);

    /**
     * 거래 기록 (TradeRecord)
     */
    void record_trade(const TradeRecord& record);

    /**
     * 거래 기록 (상세)
     */
    void record_trade(const ExtendedTradeRecord& record);

    /**
     * 현재 자산 업데이트
     */
    void update_equity(double equity_krw);

    // =========================================================================
    // 통계 조회
    // =========================================================================

    /**
     * 일간 통계
     */
    TradingStats get_daily_stats() const;

    /**
     * 주간 통계
     */
    TradingStats get_weekly_stats() const;

    /**
     * 월간 통계
     */
    TradingStats get_monthly_stats() const;

    /**
     * 연간 통계
     */
    TradingStats get_yearly_stats() const;

    /**
     * 전체 기간 통계
     */
    TradingStats get_all_time_stats() const;

    /**
     * 기간별 통계
     */
    TradingStats get_stats(StatsPeriod period) const;

    /**
     * 특정 기간 통계
     */
    TradingStats get_stats(
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end
    ) const;

    // =========================================================================
    // 일별 요약
    // =========================================================================

    /**
     * 최근 N일 일별 요약
     */
    std::vector<DailySummary> get_daily_summaries(int days = 30) const;

    /**
     * 월별 요약
     */
    std::vector<DailySummary> get_monthly_summaries(int months = 12) const;

    // =========================================================================
    // 거래 기록 조회
    // =========================================================================

    /**
     * 전체 거래 기록 수
     */
    size_t total_trade_count() const;

    /**
     * 최근 거래 기록
     */
    std::vector<ExtendedTradeRecord> get_recent_trades(size_t count = 100) const;

    /**
     * 기간별 거래 기록
     */
    std::vector<ExtendedTradeRecord> get_trades(
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end
    ) const;

    // =========================================================================
    // 파일 저장/로드
    // =========================================================================

    /**
     * 모든 데이터 저장
     */
    bool save();

    /**
     * 거래 기록 저장
     */
    bool save_trades();

    /**
     * 일별 통계 저장
     */
    bool save_daily_stats();

    /**
     * 데이터 로드
     */
    bool load();

    /**
     * 거래 기록 로드
     */
    bool load_trades();

    // =========================================================================
    // 설정
    // =========================================================================

    /**
     * 설정 업데이트
     */
    void set_config(const TradingStatsConfig& config);

    /**
     * 현재 설정
     */
    TradingStatsConfig config() const;

    /**
     * 통계 콜백 등록 (일별 마감 시)
     */
    void on_daily_close(StatsCallback callback);

    // =========================================================================
    // 리셋
    // =========================================================================

    /**
     * 통계 리셋 (거래 기록은 유지)
     */
    void reset_stats();

    /**
     * 모든 데이터 리셋
     */
    void reset_all();

private:
    // =========================================================================
    // 내부 구현
    // =========================================================================

    /**
     * 일별 마감 처리
     */
    void process_daily_close();

    /**
     * 자동 저장 타이머
     */
    void auto_save_thread();

private:
    TradingStatsConfig config_;

    // Sub-components
    StatsCalculator calculator_;
    StatsReporter reporter_;

    // 거래 기록
    mutable std::mutex trades_mutex_;
    std::deque<ExtendedTradeRecord> trades_;
    size_t total_trades_ever_{0};  // 파일에 저장된 것 포함

    // 전체 통계 (캐시)
    mutable std::mutex stats_mutex_;
    TradingStats all_time_stats_;

    // 자산 추적
    std::atomic<double> current_equity_{0.0};
    double peak_equity_{0.0};
    double max_drawdown_krw_{0.0};
    double max_drawdown_pct_{0.0};

    // 연속 기록
    int current_streak_{0};
    int max_consecutive_wins_{0};
    int max_consecutive_losses_{0};

    // 일별 요약
    std::vector<DailySummary> daily_summaries_;
    int last_processed_day_{0};

    // 콜백
    std::mutex callback_mutex_;
    StatsCallback daily_close_callback_;

    // 자동 저장
    std::atomic<bool> running_{false};
    std::thread auto_save_thread_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;
};

// =============================================================================
// 글로벌 접근자
// =============================================================================

/**
 * TradingStatsTracker 싱글톤 접근
 */
inline TradingStatsTracker& trading_stats() {
    return TradingStatsTracker::instance();
}

// =============================================================================
// 편의 함수
// =============================================================================

/**
 * 거래 기록 (글로벌)
 */
inline void record_trading_stats(double pnl_krw, double volume_krw = 0.0) {
    trading_stats().record_trade(pnl_krw, volume_krw);
}

/**
 * 일간 통계 (글로벌)
 */
inline TradingStats get_daily_trading_stats() {
    return trading_stats().get_daily_stats();
}

/**
 * 전체 통계 (글로벌)
 */
inline TradingStats get_all_time_trading_stats() {
    return trading_stats().get_all_time_stats();
}

// =============================================================================
// 문자열 변환
// =============================================================================

inline const char* to_string(StatsPeriod period) {
    switch (period) {
        case StatsPeriod::Daily:   return "Daily";
        case StatsPeriod::Weekly:  return "Weekly";
        case StatsPeriod::Monthly: return "Monthly";
        case StatsPeriod::Yearly:  return "Yearly";
        case StatsPeriod::AllTime: return "AllTime";
        default:                   return "Unknown";
    }
}

}  // namespace arbitrage
