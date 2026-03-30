#pragma once

/**
 * Decision Engine (TASK_13)
 *
 * 아비트라지 기회 평가 및 실행 결정 (오케스트레이터)
 * - 기회 평가 → OpportunityEvaluator에 위임
 * - 최적 수량 계산 → QuantityOptimizer에 위임
 * - 킬스위치, 쿨다운, 거래 이력 관리
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/strategy/risk_model.hpp"
#include "arbitrage/strategy/orderbook_analyzer.hpp"
#include "arbitrage/executor/types.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <shared_mutex>

namespace arbitrage {

// 전방 선언
class OpportunityEvaluator;
class QuantityOptimizer;

// =============================================================================
// 결정 타입
// =============================================================================
enum class Decision : uint8_t {
    Execute,    // 실행 - 모든 조건 충족
    Skip,       // 스킵 - 리스크 초과
    Wait,       // 대기 - 조건 미충족 (프리미엄 부족 등)
    HoldOff     // 보류 - 킬스위치 활성화
};

constexpr const char* decision_name(Decision d) {
    switch (d) {
        case Decision::Execute: return "Execute";
        case Decision::Skip:    return "Skip";
        case Decision::Wait:    return "Wait";
        case Decision::HoldOff: return "HoldOff";
        default:                return "Unknown";
    }
}

// =============================================================================
// 결정 사유 타입
// =============================================================================
enum class DecisionReason : uint8_t {
    None = 0,
    Approved,               // 승인됨
    KillSwitchActive,       // 킬스위치 활성화
    InsufficientPremium,    // 프리미엄 부족
    HighRiskScore,          // 리스크 점수 초과
    NegativeExpectedProfit, // 음수 기대 수익
    LowProfitProbability,   // 낮은 수익 확률
    InsufficientBalance,    // 잔액 부족
    InsufficientLiquidity,  // 유동성 부족
    HighSlippage,           // 높은 슬리피지
    MaxPositionExceeded,    // 최대 포지션 초과
    DailyLossLimitHit,      // 일일 손실 한도 도달
    CooldownActive,         // 쿨다운 활성화
    MarketClosed,           // 시장 마감
    InvalidOpportunity      // 유효하지 않은 기회
};

constexpr const char* decision_reason_name(DecisionReason r) {
    switch (r) {
        case DecisionReason::None:                  return "None";
        case DecisionReason::Approved:              return "Approved";
        case DecisionReason::KillSwitchActive:      return "KillSwitchActive";
        case DecisionReason::InsufficientPremium:   return "InsufficientPremium";
        case DecisionReason::HighRiskScore:         return "HighRiskScore";
        case DecisionReason::NegativeExpectedProfit:return "NegativeExpectedProfit";
        case DecisionReason::LowProfitProbability:  return "LowProfitProbability";
        case DecisionReason::InsufficientBalance:   return "InsufficientBalance";
        case DecisionReason::InsufficientLiquidity: return "InsufficientLiquidity";
        case DecisionReason::HighSlippage:          return "HighSlippage";
        case DecisionReason::MaxPositionExceeded:   return "MaxPositionExceeded";
        case DecisionReason::DailyLossLimitHit:     return "DailyLossLimitHit";
        case DecisionReason::CooldownActive:        return "CooldownActive";
        case DecisionReason::MarketClosed:          return "MarketClosed";
        case DecisionReason::InvalidOpportunity:    return "InvalidOpportunity";
        default:                                    return "Unknown";
    }
}

// =============================================================================
// 결정 결과
// =============================================================================
struct DecisionResult {
    Decision decision{Decision::Wait};
    DecisionReason reason{DecisionReason::None};

    // 실행 정보 (Execute 시에만 유효)
    DualOrderRequest order_request;

    // 평가 정보
    double confidence{0.0};         // 신뢰도 (0-1)
    double expected_profit_pct{0.0};// 예상 수익률 (%)
    double expected_profit_krw{0.0};// 예상 수익 (KRW)
    double risk_score{0.0};         // 리스크 점수 (0-100)
    double optimal_qty{0.0};        // 최적 수량

    // 타임스탬프
    std::chrono::steady_clock::time_point evaluated_at;

    // 결정 근거
    RiskAssessment risk_assessment;

    // 헬퍼 함수
    bool should_execute() const { return decision == Decision::Execute; }
    bool should_skip() const { return decision == Decision::Skip; }
    bool should_wait() const { return decision == Decision::Wait; }
    bool is_held_off() const { return decision == Decision::HoldOff; }

    const char* decision_str() const { return decision_name(decision); }
    const char* reason_str() const { return decision_reason_name(reason); }
};

// =============================================================================
// 전략 설정
// =============================================================================
struct StrategyConfig {
    // 프리미엄 임계값
    double min_premium_pct{0.3};        // 최소 프리미엄 (%)
    double target_premium_pct{0.5};     // 목표 프리미엄 (%)
    double max_premium_pct{5.0};        // 최대 프리미엄 (비정상 탐지)

    // 수량 설정
    double min_order_qty{10.0};         // 최소 주문 수량 (XRP)
    double max_order_qty{10000.0};      // 최대 주문 수량 (XRP)
    double base_order_qty{100.0};       // 기본 주문 수량 (XRP)

    // 잔액 설정
    double max_position_pct{50.0};      // 최대 포지션 비율 (%)
    double reserve_balance_pct{10.0};   // 예비 잔액 비율 (%)

    // 리스크 설정
    double max_risk_score{60.0};        // 최대 허용 리스크 점수
    double min_confidence{0.5};         // 최소 신뢰도
    double min_profit_probability{0.6}; // 최소 수익 확률

    // 손실 제한
    double daily_loss_limit_krw{100000.0};  // 일일 손실 한도 (KRW)
    double per_trade_loss_limit_krw{10000.0};// 건당 손실 한도 (KRW)

    // 쿨다운
    std::chrono::milliseconds cooldown_after_trade{1000};  // 거래 후 쿨다운
    std::chrono::milliseconds cooldown_after_loss{5000};   // 손실 후 쿨다운

    // 주문 타입
    OrderType default_buy_type{OrderType::Limit};
    OrderType default_sell_type{OrderType::Limit};

    // 슬리피지 허용
    double max_slippage_bps{30.0};      // 최대 슬리피지 (bps)

    // 송금 관련
    std::chrono::seconds max_transfer_time{120};  // 최대 송금 대기 시간
};

// =============================================================================
// 잔액 정보
// =============================================================================
struct BalanceInfo {
    Exchange exchange;
    double available_base{0.0};   // 가용 기준 통화 (XRP)
    double available_quote{0.0};  // 가용 견적 통화 (KRW/USDT)
    double locked_base{0.0};      // 잠긴 기준 통화
    double locked_quote{0.0};     // 잠긴 견적 통화

    double total_base() const { return available_base + locked_base; }
    double total_quote() const { return available_quote + locked_quote; }
};

// =============================================================================
// 의사결정 엔진
// =============================================================================
class DecisionEngine {
public:
    explicit DecisionEngine(const StrategyConfig& config = {});
    ~DecisionEngine();

    // 복사/이동 금지
    DecisionEngine(const DecisionEngine&) = delete;
    DecisionEngine& operator=(const DecisionEngine&) = delete;

    // =========================================================================
    // 기회 평가
    // =========================================================================

    /**
     * 아비트라지 기회 평가
     * @param opportunity 김프 기회 정보
     * @return 결정 결과
     */
    DecisionResult evaluate(const PremiumInfo& opportunity);

    /**
     * 오더북 기반 정밀 평가
     * @param opportunity 김프 기회
     * @param buy_ob 매수 거래소 오더북
     * @param sell_ob 매도 거래소 오더북
     * @return 결정 결과
     */
    DecisionResult evaluate_with_orderbook(
        const PremiumInfo& opportunity,
        const OrderBook& buy_ob,
        const OrderBook& sell_ob
    );

    // =========================================================================
    // 수량 계산
    // =========================================================================

    /**
     * 최적 주문 수량 계산
     * @param opportunity 김프 기회
     * @param risk 리스크 평가 (optional)
     * @return 최적 수량
     */
    double calculate_optimal_qty(
        const PremiumInfo& opportunity,
        const RiskAssessment* risk = nullptr
    );

    /**
     * 잔액 기반 최대 수량 계산
     * @param buy_ex 매수 거래소
     * @param sell_ex 매도 거래소
     * @param buy_price 매수 가격
     * @return 최대 가능 수량
     */
    double calculate_max_qty_by_balance(
        Exchange buy_ex,
        Exchange sell_ex,
        double buy_price
    );

    // =========================================================================
    // 킬스위치
    // =========================================================================

    void set_kill_switch(bool active);
    bool is_kill_switch_active() const;

    /**
     * 킬스위치 사유 설정
     */
    void set_kill_switch_reason(const std::string& reason);
    std::string get_kill_switch_reason() const;

    // =========================================================================
    // 잔액 관리
    // =========================================================================

    void update_balance(const BalanceInfo& balance);
    BalanceInfo get_balance(Exchange ex) const;

    // =========================================================================
    // 손익 추적
    // =========================================================================

    /**
     * 거래 결과 기록
     * @param profit_krw 손익 (KRW)
     */
    void record_trade_result(double profit_krw);

    /**
     * 일일 손익 조회
     */
    double get_daily_pnl() const { return daily_pnl_.load(); }

    /**
     * 일일 손익 리셋 (자정에 호출)
     */
    void reset_daily_pnl();

    // =========================================================================
    // 쿨다운
    // =========================================================================

    bool is_in_cooldown() const;
    std::chrono::milliseconds remaining_cooldown() const;

    /**
     * 쿨다운 시작
     * @param duration 쿨다운 시간
     */
    void start_cooldown(std::chrono::milliseconds duration);

    // =========================================================================
    // 설정
    // =========================================================================

    void set_config(const StrategyConfig& config);
    const StrategyConfig& config() const { return config_; }

    void set_risk_model(RiskModel* model) { risk_model_ = model; }
    void set_orderbook_analyzer(OrderBookAnalyzer* analyzer) { ob_analyzer_ = analyzer; }

    // =========================================================================
    // 통계
    // =========================================================================

    struct Stats {
        std::atomic<uint64_t> evaluations{0};
        std::atomic<uint64_t> executions{0};
        std::atomic<uint64_t> skips{0};
        std::atomic<uint64_t> waits{0};
        std::atomic<uint64_t> holdoffs{0};

        void reset() {
            evaluations = 0;
            executions = 0;
            skips = 0;
            waits = 0;
            holdoffs = 0;
        }

        double execution_rate() const {
            uint64_t total = evaluations.load();
            return total > 0 ? static_cast<double>(executions) / total * 100.0 : 0.0;
        }
    };

    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

    // =========================================================================
    // 디버깅
    // =========================================================================

    void print_decision(const DecisionResult& result) const;

private:
    // 전제 조건 확인 (킬스위치, 쿨다운)
    bool check_preconditions(DecisionResult& result);

    // 주문 요청 생성
    DualOrderRequest create_order_request(
        const PremiumInfo& opp,
        double qty,
        OrderType buy_type,
        OrderType sell_type
    );

    // 멤버 변수
    StrategyConfig config_;

    // 서브 컴포넌트
    std::unique_ptr<OpportunityEvaluator> evaluator_;
    std::unique_ptr<QuantityOptimizer> qty_optimizer_;

    // 킬스위치
    std::atomic<bool> kill_switch_{false};
    mutable std::shared_mutex kill_reason_mutex_;
    std::string kill_switch_reason_;

    // 잔액
    mutable std::shared_mutex balance_mutex_;
    std::array<BalanceInfo, 4> balances_;  // 거래소별 잔액

    // 손익 추적
    std::atomic<double> daily_pnl_{0.0};
    std::atomic<int> trade_count_today_{0};

    // 쿨다운 (nanoseconds since epoch)
    std::atomic<int64_t> cooldown_until_ns_{0};

    // 외부 모듈 참조
    RiskModel* risk_model_{nullptr};
    OrderBookAnalyzer* ob_analyzer_{nullptr};

    // 통계
    mutable Stats stats_;
};

// =============================================================================
// 글로벌 인스턴스 접근자
// =============================================================================
DecisionEngine& decision_engine();

}  // namespace arbitrage
