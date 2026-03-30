#pragma once

/**
 * Opportunity Evaluator
 *
 * 아비트라지 기회 평가 로직
 * - 프리미엄 임계값 검증
 * - 리스크 검증
 * - 잔액 검증
 * - 신뢰도 계산
 *
 * DecisionEngine에서 분리된 평가 전용 모듈
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/strategy/risk_model.hpp"
#include "arbitrage/strategy/decision_engine.hpp"

#include <array>
#include <shared_mutex>

namespace arbitrage {

class OpportunityEvaluator {
public:
    explicit OpportunityEvaluator(const StrategyConfig& config);

    // 설정 업데이트
    void set_config(const StrategyConfig& config);

    // =========================================================================
    // 프리미엄 검증
    // =========================================================================

    /**
     * 프리미엄 유효성 검증
     * @param opp 김프 기회
     * @param result 결정 결과 (실패 시 사유 설정)
     * @param stats 통계 카운터
     * @return true: 통과, false: 실패
     */
    bool check_premium(
        const PremiumInfo& opp,
        DecisionResult& result,
        DecisionEngine::Stats& stats
    );

    // =========================================================================
    // 리스크 검증
    // =========================================================================

    /**
     * 리스크 한도 검증
     * @param risk 리스크 평가 결과
     * @param result 결정 결과 (실패 시 사유 설정)
     * @param stats 통계 카운터
     * @return true: 통과, false: 실패
     */
    bool check_risk_limits(
        const RiskAssessment& risk,
        DecisionResult& result,
        DecisionEngine::Stats& stats
    );

    // =========================================================================
    // 잔액 검증
    // =========================================================================

    /**
     * 잔액 검증
     * @param buy_ex 매수 거래소
     * @param sell_ex 매도 거래소
     * @param qty 수량
     * @param price 가격
     * @param balances 거래소별 잔액 배열
     * @param balance_mutex 잔액 뮤텍스
     * @param result 결정 결과
     * @param stats 통계 카운터
     * @return true: 통과, false: 실패
     */
    bool check_balance(
        Exchange buy_ex,
        Exchange sell_ex,
        double qty,
        double price,
        const std::array<BalanceInfo, 4>& balances,
        std::shared_mutex& balance_mutex,
        DecisionResult& result,
        DecisionEngine::Stats& stats
    );

    // =========================================================================
    // 신뢰도 계산
    // =========================================================================

    /**
     * 신뢰도 계산
     * @param opp 김프 기회
     * @param risk 리스크 평가
     * @param qty 수량
     * @return 신뢰도 (0-1)
     */
    double calculate_confidence(
        const PremiumInfo& opp,
        const RiskAssessment& risk,
        double qty
    );

private:
    const StrategyConfig* config_;
};

}  // namespace arbitrage
