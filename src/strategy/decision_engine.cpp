/**
 * Decision Engine Implementation (TASK_13)
 */

#include "arbitrage/strategy/decision_engine.hpp"

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>

namespace arbitrage {

// =============================================================================
// 글로벌 인스턴스
// =============================================================================
DecisionEngine& decision_engine() {
    static DecisionEngine instance;
    return instance;
}

// =============================================================================
// 생성자
// =============================================================================
DecisionEngine::DecisionEngine(const StrategyConfig& config)
    : config_(config)
{
    // 잔액 초기화
    for (int i = 0; i < 4; ++i) {
        balances_[i].exchange = static_cast<Exchange>(i);
    }

    // 쿨다운 초기화 (현재 시각 = 쿨다운 없음)
    auto now = std::chrono::steady_clock::now();
    cooldown_until_ns_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
}

// =============================================================================
// 기회 평가
// =============================================================================
DecisionResult DecisionEngine::evaluate(const PremiumInfo& opportunity) {
    DecisionResult result;
    result.evaluated_at = std::chrono::steady_clock::now();
    ++stats_.evaluations;

    // 1. 전제 조건 확인 (킬스위치, 쿨다운 등)
    if (!check_preconditions(result)) {
        return result;
    }

    // 2. 기회 유효성 확인
    if (!opportunity.is_valid()) {
        result.decision = Decision::Wait;
        result.reason = DecisionReason::InvalidOpportunity;
        ++stats_.waits;
        return result;
    }

    // 3. 프리미엄 검증
    if (!check_premium(opportunity, result)) {
        return result;
    }

    // 4. 리스크 평가
    double base_qty = config_.base_order_qty;
    RiskAssessment risk;

    if (risk_model_) {
        risk = risk_model_->evaluate(
            opportunity,
            base_qty,
            config_.max_transfer_time
        );
        result.risk_assessment = risk;
        result.risk_score = risk.score;

        // 5. 리스크 검증
        if (!check_risk_limits(risk, result)) {
            return result;
        }
    }

    // 6. 최적 수량 계산
    double optimal_qty = calculate_optimal_qty(opportunity, &risk);
    result.optimal_qty = optimal_qty;

    // 7. 잔액 검증
    if (!check_balance(
            opportunity.buy_exchange,
            opportunity.sell_exchange,
            optimal_qty,
            opportunity.buy_price,
            result)) {
        return result;
    }

    // 8. 신뢰도 계산
    result.confidence = calculate_confidence(opportunity, risk, optimal_qty);

    if (result.confidence < config_.min_confidence) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::LowProfitProbability;
        ++stats_.skips;
        return result;
    }

    // 9. 예상 수익 계산
    double position_krw = optimal_qty * opportunity.sell_price;
    double fee_pct = 0.15;  // 기본 수수료 0.15%

    result.expected_profit_pct = opportunity.premium_pct - fee_pct;
    result.expected_profit_krw = position_krw * result.expected_profit_pct / 100.0;

    // 10. 손실 한도 확인
    if (result.expected_profit_krw < -config_.per_trade_loss_limit_krw) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::NegativeExpectedProfit;
        ++stats_.skips;
        return result;
    }

    // 11. 주문 요청 생성
    result.order_request = create_order_request(
        opportunity,
        optimal_qty,
        config_.default_buy_type,
        config_.default_sell_type
    );

    // 12. 최종 결정: 실행
    result.decision = Decision::Execute;
    result.reason = DecisionReason::Approved;
    ++stats_.executions;

    return result;
}

// =============================================================================
// 오더북 기반 정밀 평가
// =============================================================================
DecisionResult DecisionEngine::evaluate_with_orderbook(
    const PremiumInfo& opportunity,
    const OrderBook& buy_ob,
    const OrderBook& sell_ob
) {
    DecisionResult result;
    result.evaluated_at = std::chrono::steady_clock::now();
    ++stats_.evaluations;

    // 1. 전제 조건 확인
    if (!check_preconditions(result)) {
        return result;
    }

    // 2. 기회 유효성 확인
    if (!opportunity.is_valid()) {
        result.decision = Decision::Wait;
        result.reason = DecisionReason::InvalidOpportunity;
        ++stats_.waits;
        return result;
    }

    // 3. 프리미엄 검증
    if (!check_premium(opportunity, result)) {
        return result;
    }

    // 4. 리스크 평가 (오더북 기반)
    double base_qty = config_.base_order_qty;
    RiskAssessment risk;

    if (risk_model_) {
        risk = risk_model_->evaluate_with_orderbook(
            opportunity,
            buy_ob,
            sell_ob,
            base_qty,
            config_.max_transfer_time
        );
        result.risk_assessment = risk;
        result.risk_score = risk.score;

        // 5. 리스크 검증
        if (!check_risk_limits(risk, result)) {
            return result;
        }
    }

    // 6. 오더북 분석 (유동성, 슬리피지)
    double optimal_qty = base_qty;

    if (ob_analyzer_) {
        // 듀얼 주문 계획 생성
        DualOrderPlan plan = ob_analyzer_->plan_maker_taker_order(
            opportunity.buy_exchange,
            opportunity.sell_exchange,
            base_qty,
            opportunity.fx_rate);

        // 슬리피지 확인
        if (plan.taker_slippage_bps > config_.max_slippage_bps) {
            result.decision = Decision::Skip;
            result.reason = DecisionReason::HighSlippage;
            ++stats_.skips;
            return result;
        }

        // 유동성 기반: 수익성 확인
        if (!plan.is_profitable()) {
            result.decision = Decision::Skip;
            result.reason = DecisionReason::NegativeExpectedProfit;
            ++stats_.skips;
            return result;
        }
    }

    // 7. 포지션 사이징 적용
    optimal_qty = apply_position_sizing(optimal_qty, &risk);
    result.optimal_qty = optimal_qty;

    if (optimal_qty < config_.min_order_qty) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::InsufficientLiquidity;
        ++stats_.skips;
        return result;
    }

    // 8. 잔액 검증
    if (!check_balance(
            opportunity.buy_exchange,
            opportunity.sell_exchange,
            optimal_qty,
            opportunity.buy_price,
            result)) {
        return result;
    }

    // 9. 신뢰도 계산
    result.confidence = calculate_confidence(opportunity, risk, optimal_qty);

    if (result.confidence < config_.min_confidence) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::LowProfitProbability;
        ++stats_.skips;
        return result;
    }

    // 10. 예상 수익 계산
    double position_krw = optimal_qty * opportunity.sell_price;
    double fee_pct = 0.15;

    result.expected_profit_pct = opportunity.premium_pct - fee_pct;
    result.expected_profit_krw = position_krw * result.expected_profit_pct / 100.0;

    // 11. 주문 요청 생성
    result.order_request = create_order_request(
        opportunity,
        optimal_qty,
        config_.default_buy_type,
        config_.default_sell_type
    );

    // 12. 최종 결정
    result.decision = Decision::Execute;
    result.reason = DecisionReason::Approved;
    ++stats_.executions;

    return result;
}

// =============================================================================
// 최적 수량 계산
// =============================================================================
double DecisionEngine::calculate_optimal_qty(
    const PremiumInfo& opportunity,
    const RiskAssessment* risk
) {
    double qty = config_.base_order_qty;

    // 잔액 기반 최대 수량
    double max_by_balance = calculate_max_qty_by_balance(
        opportunity.buy_exchange,
        opportunity.sell_exchange,
        opportunity.buy_price
    );

    qty = std::min(qty, max_by_balance);

    // 포지션 사이징 적용
    qty = apply_position_sizing(qty, risk);

    // 범위 제한
    qty = std::clamp(qty, config_.min_order_qty, config_.max_order_qty);

    return qty;
}

// =============================================================================
// 잔액 기반 최대 수량
// =============================================================================
double DecisionEngine::calculate_max_qty_by_balance(
    Exchange buy_ex,
    Exchange sell_ex,
    double buy_price
) {
    std::shared_lock lock(balance_mutex_);

    const auto& buy_balance = balances_[static_cast<int>(buy_ex)];
    const auto& sell_balance = balances_[static_cast<int>(sell_ex)];

    // 매수: quote 통화 (KRW/USDT) 필요
    // 예비금 제외
    double available_quote = buy_balance.available_quote *
                             (100.0 - config_.reserve_balance_pct) / 100.0;
    double max_by_buy = buy_price > 0 ? available_quote / buy_price : 0.0;

    // 매도: base 통화 (XRP) 필요
    double available_base = sell_balance.available_base *
                            (100.0 - config_.reserve_balance_pct) / 100.0;
    double max_by_sell = available_base;

    return std::min(max_by_buy, max_by_sell);
}

// =============================================================================
// 킬스위치
// =============================================================================
void DecisionEngine::set_kill_switch(bool active) {
    kill_switch_.store(active, std::memory_order_release);
}

bool DecisionEngine::is_kill_switch_active() const {
    return kill_switch_.load(std::memory_order_acquire);
}

void DecisionEngine::set_kill_switch_reason(const std::string& reason) {
    std::unique_lock lock(kill_reason_mutex_);
    kill_switch_reason_ = reason;
}

std::string DecisionEngine::get_kill_switch_reason() const {
    std::shared_lock lock(kill_reason_mutex_);
    return kill_switch_reason_;
}

// =============================================================================
// 잔액 관리
// =============================================================================
void DecisionEngine::update_balance(const BalanceInfo& balance) {
    std::unique_lock lock(balance_mutex_);
    int idx = static_cast<int>(balance.exchange);
    if (idx >= 0 && idx < 4) {
        balances_[idx] = balance;
    }
}

BalanceInfo DecisionEngine::get_balance(Exchange ex) const {
    std::shared_lock lock(balance_mutex_);
    return balances_[static_cast<int>(ex)];
}

// =============================================================================
// 손익 추적
// =============================================================================
void DecisionEngine::record_trade_result(double profit_krw) {
    // Atomic addition for double using CAS
    double current = daily_pnl_.load();
    while (!daily_pnl_.compare_exchange_weak(current, current + profit_krw));

    ++trade_count_today_;

    // 손실 발생 시 쿨다운
    if (profit_krw < 0) {
        start_cooldown(config_.cooldown_after_loss);
    } else {
        start_cooldown(config_.cooldown_after_trade);
    }

    // 일일 손실 한도 확인
    if (daily_pnl_.load() < -config_.daily_loss_limit_krw) {
        set_kill_switch(true);
        set_kill_switch_reason("Daily loss limit reached");
    }
}

void DecisionEngine::reset_daily_pnl() {
    daily_pnl_.store(0.0);
    trade_count_today_.store(0);
}

// =============================================================================
// 쿨다운
// =============================================================================
bool DecisionEngine::is_in_cooldown() const {
    auto now = std::chrono::steady_clock::now();
    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    return now_ns < cooldown_until_ns_.load();
}

std::chrono::milliseconds DecisionEngine::remaining_cooldown() const {
    auto now = std::chrono::steady_clock::now();
    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    int64_t until_ns = cooldown_until_ns_.load();

    if (now_ns >= until_ns) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::milliseconds((until_ns - now_ns) / 1000000);
}

void DecisionEngine::start_cooldown(std::chrono::milliseconds duration) {
    auto new_until = std::chrono::steady_clock::now() + duration;
    int64_t new_until_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        new_until.time_since_epoch()).count();
    int64_t current = cooldown_until_ns_.load();

    // 더 긴 쿨다운으로만 업데이트
    while (new_until_ns > current &&
           !cooldown_until_ns_.compare_exchange_weak(current, new_until_ns));
}

// =============================================================================
// 설정
// =============================================================================
void DecisionEngine::set_config(const StrategyConfig& config) {
    config_ = config;
}

// =============================================================================
// 전제 조건 확인
// =============================================================================
bool DecisionEngine::check_preconditions(DecisionResult& result) {
    // 킬스위치 확인
    if (is_kill_switch_active()) {
        result.decision = Decision::HoldOff;
        result.reason = DecisionReason::KillSwitchActive;
        ++stats_.holdoffs;
        return false;
    }

    // 쿨다운 확인
    if (is_in_cooldown()) {
        result.decision = Decision::Wait;
        result.reason = DecisionReason::CooldownActive;
        ++stats_.waits;
        return false;
    }

    // 일일 손실 한도 확인
    if (daily_pnl_.load() < -config_.daily_loss_limit_krw) {
        result.decision = Decision::HoldOff;
        result.reason = DecisionReason::DailyLossLimitHit;
        ++stats_.holdoffs;
        return false;
    }

    return true;
}

// =============================================================================
// 프리미엄 검증
// =============================================================================
bool DecisionEngine::check_premium(const PremiumInfo& opp, DecisionResult& result) {
    // 최소 프리미엄 확인
    if (opp.premium_pct < config_.min_premium_pct) {
        result.decision = Decision::Wait;
        result.reason = DecisionReason::InsufficientPremium;
        ++stats_.waits;
        return false;
    }

    // 비정상 프리미엄 확인 (너무 높으면 의심)
    if (opp.premium_pct > config_.max_premium_pct) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::InvalidOpportunity;
        ++stats_.skips;
        return false;
    }

    return true;
}

// =============================================================================
// 리스크 검증
// =============================================================================
bool DecisionEngine::check_risk_limits(const RiskAssessment& risk, DecisionResult& result) {
    // 리스크 점수 확인
    if (risk.score > config_.max_risk_score) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::HighRiskScore;
        ++stats_.skips;
        return false;
    }

    // 기대 수익 확인
    if (risk.expected_profit_pct < 0) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::NegativeExpectedProfit;
        ++stats_.skips;
        return false;
    }

    // 수익 확률 확인
    if (risk.profit_probability < config_.min_profit_probability) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::LowProfitProbability;
        ++stats_.skips;
        return false;
    }

    return true;
}

// =============================================================================
// 잔액 검증
// =============================================================================
bool DecisionEngine::check_balance(
    Exchange buy_ex,
    Exchange sell_ex,
    double qty,
    double price,
    DecisionResult& result
) {
    std::shared_lock lock(balance_mutex_);

    const auto& buy_balance = balances_[static_cast<int>(buy_ex)];
    const auto& sell_balance = balances_[static_cast<int>(sell_ex)];

    // 매수에 필요한 quote 통화
    double required_quote = qty * price;
    if (buy_balance.available_quote < required_quote) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::InsufficientBalance;
        ++stats_.skips;
        return false;
    }

    // 매도에 필요한 base 통화
    if (sell_balance.available_base < qty) {
        result.decision = Decision::Skip;
        result.reason = DecisionReason::InsufficientBalance;
        ++stats_.skips;
        return false;
    }

    return true;
}

// =============================================================================
// 포지션 사이징
// =============================================================================
double DecisionEngine::apply_position_sizing(double base_qty, const RiskAssessment* risk) {
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
    qty = std::clamp(qty, config_.min_order_qty, config_.max_order_qty);

    return qty;
}

// =============================================================================
// 주문 요청 생성
// =============================================================================
DualOrderRequest DecisionEngine::create_order_request(
    const PremiumInfo& opp,
    double qty,
    OrderType buy_type,
    OrderType sell_type
) {
    DualOrderRequest request;
    request.set_request_id_auto();
    request.expected_premium = opp.premium_pct;

    // 매수 주문 (해외 거래소)
    request.buy_order.exchange = opp.buy_exchange;
    request.buy_order.set_symbol("XRP");
    request.buy_order.side = OrderSide::Buy;
    request.buy_order.type = buy_type;
    request.buy_order.quantity = qty;
    request.buy_order.price = opp.buy_price / opp.fx_rate;  // USDT 가격

    // 매도 주문 (국내 거래소)
    request.sell_order.exchange = opp.sell_exchange;
    request.sell_order.set_symbol("XRP");
    request.sell_order.side = OrderSide::Sell;
    request.sell_order.type = sell_type;
    request.sell_order.quantity = qty;
    request.sell_order.price = opp.sell_price;  // KRW 가격

    return request;
}

// =============================================================================
// 신뢰도 계산
// =============================================================================
double DecisionEngine::calculate_confidence(
    const PremiumInfo& opp,
    const RiskAssessment& risk,
    double qty
) {
    double confidence = 0.5;  // 기본값

    // 프리미엄 기반 신뢰도 (목표 대비)
    double premium_factor = opp.premium_pct / config_.target_premium_pct;
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

// =============================================================================
// 디버깅 출력
// =============================================================================
void DecisionEngine::print_decision(const DecisionResult& result) const {
    std::cout << "\n========== Decision Result ==========\n";
    std::cout << std::fixed << std::setprecision(4);

    std::cout << "Decision: " << result.decision_str() << "\n";
    std::cout << "Reason:   " << result.reason_str() << "\n\n";

    std::cout << "[Evaluation]\n";
    std::cout << "  Confidence:       " << (result.confidence * 100) << "%\n";
    std::cout << "  Risk Score:       " << result.risk_score << "/100\n";
    std::cout << "  Optimal Qty:      " << result.optimal_qty << " XRP\n\n";

    std::cout << "[Expected Profit]\n";
    std::cout << "  Profit %:         " << result.expected_profit_pct << "%\n";
    std::cout << "  Profit KRW:       " << result.expected_profit_krw << " KRW\n\n";

    if (result.should_execute()) {
        std::cout << "[Order Request]\n";
        std::cout << "  Buy Exchange:     " << exchange_name(result.order_request.buy_order.exchange) << "\n";
        std::cout << "  Buy Price:        " << result.order_request.buy_order.price << "\n";
        std::cout << "  Sell Exchange:    " << exchange_name(result.order_request.sell_order.exchange) << "\n";
        std::cout << "  Sell Price:       " << result.order_request.sell_order.price << "\n";
        std::cout << "  Quantity:         " << result.order_request.buy_order.quantity << " XRP\n";
    }

    std::cout << "======================================\n";
}

}  // namespace arbitrage
