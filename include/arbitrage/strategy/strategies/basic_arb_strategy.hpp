#pragma once

/**
 * Basic Arbitrage Strategy (TASK_14)
 *
 * 기본 김치 프리미엄 아비트라지 전략
 * - 해외에서 매수, 국내에서 매도
 * - Taker + Taker 방식
 */

#include "arbitrage/strategy/strategy_interface.hpp"
#include "arbitrage/strategy/strategy_registry.hpp"

namespace arbitrage {

// =============================================================================
// 기본 김프 아비트라지 전략
// =============================================================================
class BasicArbStrategy : public StrategyBase {
public:
    BasicArbStrategy();
    ~BasicArbStrategy() override = default;

    // =========================================================================
    // 식별
    // =========================================================================
    const char* name() const override { return "Basic Kimchi Premium Arbitrage"; }
    const char* description() const override {
        return "Simple Taker+Taker arbitrage: Buy overseas, Sell domestic";
    }

    // =========================================================================
    // 초기화
    // =========================================================================
    void initialize(const StrategyConfig& config) override;

    // =========================================================================
    // 핵심: 기회 평가
    // =========================================================================
    StrategyDecision evaluate(const MarketSnapshot& snapshot) override;

private:
    // 최고 김프 기회 탐색
    struct Opportunity {
        Exchange buy_exchange;
        Exchange sell_exchange;
        double premium_pct;
        double buy_price;
        double sell_price;
        bool valid{false};
    };

    Opportunity find_best_opportunity(const MarketSnapshot& snapshot) const;

    // 주문 수량 계산
    double calculate_order_qty(
        const MarketSnapshot& snapshot,
        const Opportunity& opp
    ) const;

    // 예상 수익 계산
    double calculate_expected_profit(
        double qty,
        const Opportunity& opp,
        double fee_pct
    ) const;

    // 주문 요청 생성
    DualOrderRequest create_order_request(
        const Opportunity& opp,
        double qty
    ) const;

    // 기본 파라미터
    static constexpr double DEFAULT_MIN_PREMIUM_PCT = 0.5;
    static constexpr double DEFAULT_MAX_POSITION_XRP = 5000.0;
    static constexpr double DEFAULT_FEE_PCT = 0.15;  // 0.15%
    static constexpr double DEFAULT_MIN_ORDER_QTY = 10.0;
};

// 레지스트리에 자동 등록
REGISTER_STRATEGY("basic_arb", BasicArbStrategy);

}  // namespace arbitrage
