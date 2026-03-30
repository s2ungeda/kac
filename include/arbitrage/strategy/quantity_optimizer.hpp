#pragma once

/**
 * Quantity Optimizer
 *
 * 최적 주문 수량 계산 로직
 * - 기본 수량 계산
 * - 잔액 기반 최대 수량
 * - 포지션 사이징 (리스크 조정)
 * - 슬리피지 조정 수량
 *
 * DecisionEngine에서 분리된 수량 최적화 전용 모듈
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/strategy/risk_model.hpp"
#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/strategy/decision_engine.hpp"

#include <array>
#include <shared_mutex>

namespace arbitrage {

class QuantityOptimizer {
public:
    explicit QuantityOptimizer(const StrategyConfig& config);

    // 설정 업데이트
    void set_config(const StrategyConfig& config);

    // =========================================================================
    // 최적 수량 계산
    // =========================================================================

    /**
     * 최적 주문 수량 계산
     * @param opportunity 김프 기회
     * @param risk 리스크 평가 (optional)
     * @param balances 거래소별 잔액 배열
     * @param balance_mutex 잔액 뮤텍스
     * @return 최적 수량
     */
    double calculate_optimal_qty(
        const PremiumInfo& opportunity,
        const RiskAssessment* risk,
        const std::array<BalanceInfo, 4>& balances,
        std::shared_mutex& balance_mutex
    );

    // =========================================================================
    // 잔액 기반 최대 수량
    // =========================================================================

    /**
     * 잔액 기반 최대 수량 계산
     * @param buy_ex 매수 거래소
     * @param sell_ex 매도 거래소
     * @param buy_price 매수 가격
     * @param balances 거래소별 잔액 배열
     * @param balance_mutex 잔액 뮤텍스
     * @return 최대 가능 수량
     */
    double calculate_max_qty_by_balance(
        Exchange buy_ex,
        Exchange sell_ex,
        double buy_price,
        const std::array<BalanceInfo, 4>& balances,
        std::shared_mutex& balance_mutex
    );

    // =========================================================================
    // 포지션 사이징
    // =========================================================================

    /**
     * 리스크 기반 포지션 사이징 적용
     * @param base_qty 기본 수량
     * @param risk 리스크 평가 (optional)
     * @return 조정된 수량
     */
    double apply_position_sizing(double base_qty, const RiskAssessment* risk);

private:
    const StrategyConfig* config_;
};

}  // namespace arbitrage
