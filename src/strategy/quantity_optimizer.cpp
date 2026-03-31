/**
 * Quantity Optimizer Implementation
 *
 * DecisionEngine에서 분리된 수량 최적화 로직
 */

#include "arbitrage/strategy/quantity_optimizer.hpp"

#include <algorithm>
#include <cmath>

namespace arbitrage {

// =============================================================================
// 생성자
// =============================================================================
QuantityOptimizer::QuantityOptimizer(const StrategyConfig& config)
    : config_(&config)
{
}

// =============================================================================
// 설정 업데이트
// =============================================================================
void QuantityOptimizer::set_config(const StrategyConfig& config) {
    config_ = &config;
}

// =============================================================================
// 최적 수량 계산
// =============================================================================
double QuantityOptimizer::calculate_optimal_qty(
    const PremiumInfo& opportunity,
    const RiskAssessment* risk,
    const std::array<BalanceInfo, 4>& balances,
    RWSpinLock& balance_mutex
) {
    double qty = config_->base_order_qty;

    // 잔액 기반 최대 수량
    double max_by_balance = calculate_max_qty_by_balance(
        opportunity.buy_exchange,
        opportunity.sell_exchange,
        opportunity.buy_price,
        balances,
        balance_mutex
    );

    qty = std::min(qty, max_by_balance);

    // 포지션 사이징 적용
    qty = apply_position_sizing(qty, risk);

    // 범위 제한
    qty = std::clamp(qty, config_->min_order_qty, config_->max_order_qty);

    return qty;
}

// =============================================================================
// 잔액 기반 최대 수량
// =============================================================================
double QuantityOptimizer::calculate_max_qty_by_balance(
    Exchange buy_ex,
    Exchange sell_ex,
    double buy_price,
    const std::array<BalanceInfo, 4>& balances,
    RWSpinLock& balance_mutex
) {
    ReadGuard lock(balance_mutex);

    const auto& buy_balance = balances[static_cast<int>(buy_ex)];
    const auto& sell_balance = balances[static_cast<int>(sell_ex)];

    // 매수: quote 통화 (KRW/USDT) 필요
    // 예비금 제외
    double available_quote = buy_balance.available_quote *
                             (100.0 - config_->reserve_balance_pct) / 100.0;
    double max_by_buy = buy_price > 0 ? available_quote / buy_price : 0.0;

    // 매도: base 통화 (XRP) 필요
    double available_base = sell_balance.available_base *
                            (100.0 - config_->reserve_balance_pct) / 100.0;
    double max_by_sell = available_base;

    return std::min(max_by_buy, max_by_sell);
}

// =============================================================================
// 포지션 사이징
// =============================================================================
double QuantityOptimizer::apply_position_sizing(double base_qty, const RiskAssessment* risk) {
    double qty = base_qty;

    if (risk) {
        // 리스크에 따른 수량 조정
        // 리스크 높을수록 수량 감소
        double risk_factor = 1.0 - (risk->score / 100.0) * 0.5;
        qty *= risk_factor;

        // 수익 확률에 따른 조정
        qty *= risk->profit_probability;
    }

    // 최소/최대 제한
    qty = std::clamp(qty, config_->min_order_qty, config_->max_order_qty);

    return qty;
}

}  // namespace arbitrage
