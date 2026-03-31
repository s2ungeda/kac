/**
 * Opportunity Evaluator Implementation
 *
 * DecisionEngine에서 분리된 평가 로직
 */

#include "arbitrage/strategy/opportunity_evaluator.hpp"

#include <algorithm>
#include <cmath>

namespace arbitrage {

// =============================================================================
// 생성자
// =============================================================================
OpportunityEvaluator::OpportunityEvaluator(const StrategyConfig& config)
    : config_(&config)
{
}

// =============================================================================
// 설정 업데이트
// =============================================================================
void OpportunityEvaluator::set_config(const StrategyConfig& config) {
    config_ = &config;
}

// =============================================================================
// 프리미엄 검증
// =============================================================================
bool OpportunityEvaluator::check_premium(
    const PremiumInfo& opp,
    DecisionResult& result,
    DecisionEngine::Stats& stats
) {
    // 최소 프리미엄 확인
    if (opp.premium_pct < config_->min_premium_pct) {
        result.decision = Decision::Wait;
        result.reason = DecisionReason::InsufficientPremium;
        ++stats.waits;
        return false;
    }

    // 비정상 프리미엄 확인 (너무 높으면 의심)
    if (opp.premium_pct > config_->max_premium_pct) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::InvalidOpportunity;
        ++stats.skips;
        return false;
    }

    return true;
}

// =============================================================================
// 리스크 검증
// =============================================================================
bool OpportunityEvaluator::check_risk_limits(
    const RiskAssessment& risk,
    DecisionResult& result,
    DecisionEngine::Stats& stats
) {
    // 리스크 점수 확인
    if (risk.score > config_->max_risk_score) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::HighRiskScore;
        ++stats.skips;
        return false;
    }

    // 기대 수익 확인
    if (risk.expected_profit_pct < 0) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::NegativeExpectedProfit;
        ++stats.skips;
        return false;
    }

    // 수익 확률 확인
    if (risk.profit_probability < config_->min_profit_probability) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::LowProfitProbability;
        ++stats.skips;
        return false;
    }

    return true;
}

// =============================================================================
// 잔액 검증
// =============================================================================
bool OpportunityEvaluator::check_balance(
    Exchange buy_ex,
    Exchange sell_ex,
    double qty,
    double price,
    const std::array<BalanceInfo, 4>& balances,
    RWSpinLock& balance_mutex,
    DecisionResult& result,
    DecisionEngine::Stats& stats
) {
    ReadGuard lock(balance_mutex);

    const auto& buy_balance = balances[static_cast<int>(buy_ex)];
    const auto& sell_balance = balances[static_cast<int>(sell_ex)];

    // 매수에 필요한 quote 통화
    double required_quote = qty * price;
    if (buy_balance.available_quote < required_quote) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::InsufficientBalance;
        ++stats.skips;
        return false;
    }

    // 매도에 필요한 base 통화
    if (sell_balance.available_base < qty) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::InsufficientBalance;
        ++stats.skips;
        return false;
    }

    return true;
}

// =============================================================================
// 신뢰도 계산
// =============================================================================
double OpportunityEvaluator::calculate_confidence(
    const PremiumInfo& opp,
    const RiskAssessment& risk,
    [[maybe_unused]] double qty
) {
    double confidence = 0.5;  // 기본값

    // 프리미엄 기반 신뢰도 (목표 대비)
    double premium_factor = opp.premium_pct / config_->target_premium_pct;
    premium_factor = std::min(premium_factor, 2.0);  // 최대 2배
    confidence += premium_factor * 0.15;

    // 리스크 기반 신뢰도
    double risk_factor = 1.0 - risk.score / 100.0;
    confidence += risk_factor * 0.2;

    // 수익 확률 반영
    confidence += risk.profit_probability * 0.15;

    // 범위 제한
    confidence = std::clamp(confidence, 0.0, 1.0);

    return confidence;
}

}  // namespace arbitrage
