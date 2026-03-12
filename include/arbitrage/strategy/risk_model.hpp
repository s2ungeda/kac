#pragma once

/**
 * Risk Model (TASK_12)
 *
 * 아비트라지 리스크 평가 모델
 * - 송금 리스크 (시간, 네트워크 상태)
 * - 시장 리스크 (김프 변동성, 슬리피지)
 * - VaR (Value at Risk) 계산
 * - 종합 리스크 점수
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/strategy/slippage_model.hpp"
#include "arbitrage/common/fee_calculator.hpp"

#include <deque>
#include <vector>
#include <string>
#include <chrono>
#include <array>
#include <shared_mutex>
#include <atomic>
#include <functional>
#include <cmath>

namespace arbitrage {

// =============================================================================
// 리스크 레벨
// =============================================================================
enum class RiskLevel : uint8_t {
    Low,        // 0-30: 안전
    Medium,     // 31-60: 주의
    High,       // 61-80: 위험
    Critical    // 81-100: 매우 위험
};

constexpr const char* risk_level_name(RiskLevel level) {
    switch (level) {
        case RiskLevel::Low:      return "Low";
        case RiskLevel::Medium:   return "Medium";
        case RiskLevel::High:     return "High";
        case RiskLevel::Critical: return "Critical";
        default:                  return "Unknown";
    }
}

// =============================================================================
// 리스크 경고 타입
// =============================================================================
enum class RiskWarning : uint8_t {
    None = 0,
    LowLiquidity,           // 유동성 부족
    HighVolatility,         // 높은 변동성
    LongTransferTime,       // 송금 시간 지연
    NetworkCongestion,      // 네트워크 혼잡
    HighSlippage,           // 높은 슬리피지
    LowPremium,             // 낮은 프리미엄
    NegativeExpectedProfit, // 음수 기대 수익
    ExchangeRisk,           // 거래소 위험
    PartialFillRisk,        // 부분 체결 위험
    MarginTooThin           // 마진 너무 낮음
};

constexpr const char* risk_warning_name(RiskWarning warning) {
    switch (warning) {
        case RiskWarning::None:                   return "None";
        case RiskWarning::LowLiquidity:           return "LowLiquidity";
        case RiskWarning::HighVolatility:         return "HighVolatility";
        case RiskWarning::LongTransferTime:       return "LongTransferTime";
        case RiskWarning::NetworkCongestion:      return "NetworkCongestion";
        case RiskWarning::HighSlippage:           return "HighSlippage";
        case RiskWarning::LowPremium:             return "LowPremium";
        case RiskWarning::NegativeExpectedProfit: return "NegativeExpectedProfit";
        case RiskWarning::ExchangeRisk:           return "ExchangeRisk";
        case RiskWarning::PartialFillRisk:        return "PartialFillRisk";
        case RiskWarning::MarginTooThin:          return "MarginTooThin";
        default:                                  return "Unknown";
    }
}

// =============================================================================
// 리스크 평가 결과
// =============================================================================
struct RiskAssessment {
    // 종합 점수 (0-100, 높을수록 위험)
    double score{0.0};
    RiskLevel level{RiskLevel::Low};

    // 수익/손실 분석
    double expected_profit_krw{0.0};   // 예상 수익 (KRW)
    double expected_profit_pct{0.0};   // 예상 수익률 (%)
    double max_loss_krw{0.0};          // 최대 손실 (KRW)
    double max_loss_pct{0.0};          // 최대 손실률 (%)

    // VaR (Value at Risk)
    double var_95_krw{0.0};            // 95% VaR (KRW)
    double var_99_krw{0.0};            // 99% VaR (KRW)
    double var_95_pct{0.0};            // 95% VaR (%)

    // 확률 추정
    double profit_probability{0.0};    // 수익 확률 (0-1)
    double full_fill_probability{0.0}; // 전량 체결 확률

    // 개별 리스크 점수
    double transfer_risk{0.0};         // 송금 리스크 (0-100)
    double market_risk{0.0};           // 시장 리스크 (0-100)
    double liquidity_risk{0.0};        // 유동성 리스크 (0-100)
    double slippage_risk{0.0};         // 슬리피지 리스크 (0-100)

    // 결정
    bool is_acceptable{false};         // 허용 가능 여부
    bool should_execute{false};        // 실행 권장 여부

    // 경고 목록
    static constexpr int MAX_WARNINGS = 10;
    RiskWarning warnings[MAX_WARNINGS]{};
    int warning_count{0};

    // 경고 추가
    void add_warning(RiskWarning w) {
        if (warning_count < MAX_WARNINGS) {
            warnings[warning_count++] = w;
        }
    }

    bool has_warning(RiskWarning w) const {
        for (int i = 0; i < warning_count; ++i) {
            if (warnings[i] == w) return true;
        }
        return false;
    }

    // 리스크 레벨 결정
    void update_level() {
        if (score <= 30) level = RiskLevel::Low;
        else if (score <= 60) level = RiskLevel::Medium;
        else if (score <= 80) level = RiskLevel::High;
        else level = RiskLevel::Critical;
    }
};

// =============================================================================
// 김프 변동성 통계
// =============================================================================
struct PremiumStats {
    double mean{0.0};           // 평균
    double std_dev{0.0};        // 표준편차
    double min{0.0};            // 최소
    double max{0.0};            // 최대
    double current{0.0};        // 현재값
    double z_score{0.0};        // Z-점수 (현재값의 이상치 정도)
    size_t sample_count{0};     // 샘플 수
};

// =============================================================================
// 송금 시간 통계
// =============================================================================
struct TransferTimeStats {
    Exchange from{Exchange::Binance};
    Exchange to{Exchange::Upbit};

    double avg_seconds{0.0};    // 평균 송금 시간 (초)
    double std_dev{0.0};        // 표준편차
    double p95_seconds{0.0};    // 95 퍼센타일
    double p99_seconds{0.0};    // 99 퍼센타일
    size_t sample_count{0};     // 샘플 수

    // 기본값 (XRP 기준)
    static constexpr double DEFAULT_AVG_SEC = 10.0;      // 평균 10초
    static constexpr double DEFAULT_P95_SEC = 30.0;      // 95% 30초
    static constexpr double DEFAULT_P99_SEC = 60.0;      // 99% 1분
};

// =============================================================================
// 리스크 모델 설정
// =============================================================================
struct RiskModelConfig {
    // 리스크 임계값
    double max_acceptable_score{60.0};     // 최대 허용 리스크 점수
    double min_expected_profit_pct{0.1};   // 최소 기대 수익률 (%)
    double max_var_95_pct{2.0};            // 최대 VaR 95% (%)

    // 가중치 (합계 = 1.0)
    double weight_transfer{0.25};          // 송금 리스크 가중치
    double weight_market{0.30};            // 시장 리스크 가중치
    double weight_liquidity{0.25};         // 유동성 리스크 가중치
    double weight_slippage{0.20};          // 슬리피지 리스크 가중치

    // 김프 변동성 설정
    size_t premium_history_size{1000};     // 김프 히스토리 크기
    double high_volatility_threshold{0.5}; // 높은 변동성 임계값 (%)

    // 송금 설정
    double max_transfer_time_sec{120.0};   // 최대 허용 송금 시간 (초)
    double transfer_risk_per_min{10.0};    // 분당 송금 리스크 증가량

    // 슬리피지 설정
    double max_slippage_bps{50.0};         // 최대 허용 슬리피지 (bps)
};

// =============================================================================
// 리스크 모델
// =============================================================================
class RiskModel {
public:
    explicit RiskModel(const RiskModelConfig& config = {});
    ~RiskModel() = default;

    // 복사/이동 금지
    RiskModel(const RiskModel&) = delete;
    RiskModel& operator=(const RiskModel&) = delete;

    // =========================================================================
    // 종합 리스크 평가
    // =========================================================================

    /**
     * 아비트라지 기회 리스크 평가
     * @param opportunity 김프 기회 정보
     * @param order_qty 주문 수량
     * @param estimated_transfer_time 예상 송금 시간
     * @return 리스크 평가 결과
     */
    RiskAssessment evaluate(
        const PremiumInfo& opportunity,
        double order_qty,
        std::chrono::seconds estimated_transfer_time = std::chrono::seconds(30)
    );

    /**
     * 리스크 평가 (오더북 기반, 더 정밀)
     * @param opportunity 김프 기회
     * @param buy_ob 매수 거래소 오더북
     * @param sell_ob 매도 거래소 오더북
     * @param order_qty 주문 수량
     * @param estimated_transfer_time 예상 송금 시간
     * @return 리스크 평가 결과
     */
    RiskAssessment evaluate_with_orderbook(
        const PremiumInfo& opportunity,
        const OrderBook& buy_ob,
        const OrderBook& sell_ob,
        double order_qty,
        std::chrono::seconds estimated_transfer_time = std::chrono::seconds(30)
    );

    // =========================================================================
    // 개별 리스크 계산
    // =========================================================================

    /**
     * 송금 리스크 계산 (0-100)
     * @param from 출발 거래소
     * @param to 도착 거래소
     * @param transfer_time 예상 송금 시간
     * @return 리스크 점수
     */
    double calculate_transfer_risk(
        Exchange from,
        Exchange to,
        std::chrono::seconds transfer_time
    ) const;

    /**
     * 시장 리스크 계산 (0-100)
     * - 김프 변동성 기반
     * @param opportunity 김프 기회
     * @return 리스크 점수
     */
    double calculate_market_risk(const PremiumInfo& opportunity) const;

    /**
     * 슬리피지 기반 리스크 (0-100)
     * @param estimate 슬리피지 예측 결과
     * @return 리스크 점수
     */
    double calculate_slippage_risk(const SlippageEstimate& estimate) const;

    // =========================================================================
    // 김프 변동성 분석
    // =========================================================================

    /**
     * 김프 데이터 기록
     * @param buy_ex 매수 거래소
     * @param sell_ex 매도 거래소
     * @param premium_pct 김프 (%)
     */
    void record_premium(Exchange buy_ex, Exchange sell_ex, double premium_pct);

    /**
     * 김프 변동성 계산
     * @param buy_ex 매수 거래소
     * @param sell_ex 매도 거래소
     * @return 변동성 통계
     */
    PremiumStats get_premium_stats(Exchange buy_ex, Exchange sell_ex) const;

    /**
     * 전체 김프 변동성 (모든 경로 평균)
     */
    double calculate_overall_volatility() const;

    // =========================================================================
    // 송금 시간 통계
    // =========================================================================

    /**
     * 송금 시간 기록
     * @param from 출발 거래소
     * @param to 도착 거래소
     * @param elapsed 실제 소요 시간
     */
    void record_transfer_time(
        Exchange from,
        Exchange to,
        std::chrono::seconds elapsed
    );

    /**
     * 송금 시간 통계 조회
     */
    TransferTimeStats get_transfer_stats(Exchange from, Exchange to) const;

    // =========================================================================
    // VaR 계산
    // =========================================================================

    /**
     * Value at Risk 계산
     * @param position_krw 포지션 금액 (KRW)
     * @param confidence 신뢰 수준 (0.95 = 95%)
     * @param time_horizon 시간 지평 (분)
     * @return VaR (KRW)
     */
    double calculate_var(
        double position_krw,
        double confidence = 0.95,
        int time_horizon_min = 5
    ) const;

    // =========================================================================
    // 설정
    // =========================================================================

    void set_config(const RiskModelConfig& config);
    const RiskModelConfig& config() const { return config_; }

    void set_max_acceptable_score(double score) {
        config_.max_acceptable_score = score;
    }

    void set_fee_calculator(FeeCalculator* calc) { fee_calc_ = calc; }
    void set_slippage_model(SlippageModel* model) { slippage_model_ = model; }

    // =========================================================================
    // 통계
    // =========================================================================

    struct Stats {
        std::atomic<uint64_t> evaluations{0};
        std::atomic<uint64_t> accepted{0};
        std::atomic<uint64_t> rejected{0};

        void reset() {
            evaluations = 0;
            accepted = 0;
            rejected = 0;
        }

        double acceptance_rate() const {
            uint64_t total = evaluations.load();
            return total > 0 ? static_cast<double>(accepted) / total : 0.0;
        }
    };

    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

    // =========================================================================
    // 유틸리티
    // =========================================================================

    /**
     * 리스크 평가 요약 출력
     */
    void print_assessment(const RiskAssessment& assessment) const;

private:
    // 김프 히스토리 키 생성
    static int premium_key(Exchange buy, Exchange sell) {
        return static_cast<int>(buy) * 4 + static_cast<int>(sell);
    }

    // 표준편차 계산
    static double calc_std_dev(const std::deque<double>& data, double mean);

    // Z-점수 계산
    static double calc_z_score(double value, double mean, double std_dev) {
        return std_dev > 0.0 ? (value - mean) / std_dev : 0.0;
    }

    // 정규분포 역함수 (Z값 -> 백분위)
    static double inverse_normal_cdf(double p);

    // 멤버 변수
    RiskModelConfig config_;

    // 김프 히스토리 [buy*4 + sell]
    std::array<std::deque<double>, 16> premium_history_;

    // 송금 시간 히스토리 [from*4 + to]
    std::array<std::deque<double>, 16> transfer_time_history_;

    // 외부 모듈 참조
    FeeCalculator* fee_calc_{nullptr};
    SlippageModel* slippage_model_{nullptr};

    // 동기화
    mutable std::shared_mutex mutex_;

    // 통계
    mutable Stats stats_;
};

// =============================================================================
// 글로벌 인스턴스 접근자
// =============================================================================
RiskModel& risk_model();

}  // namespace arbitrage
