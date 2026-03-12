/**
 * Basic Arbitrage Strategy Implementation (TASK_14)
 */

#include "arbitrage/strategy/strategies/basic_arb_strategy.hpp"

#include <cmath>
#include <algorithm>

namespace arbitrage {

// =============================================================================
// 생성자
// =============================================================================
BasicArbStrategy::BasicArbStrategy() {
    // 기본 파라미터 설정
    config_.params.set("min_premium_pct", DEFAULT_MIN_PREMIUM_PCT);
    config_.params.set("max_position_xrp", DEFAULT_MAX_POSITION_XRP);
    config_.params.set("fee_pct", DEFAULT_FEE_PCT);
    config_.params.set("min_order_qty", DEFAULT_MIN_ORDER_QTY);
}

// =============================================================================
// 초기화
// =============================================================================
void BasicArbStrategy::initialize(const StrategyConfig& config) {
    StrategyBase::initialize(config);

    // 사용자 설정으로 기본값 덮어쓰기
    if (!config.params.has("min_premium_pct")) {
        config_.params.set("min_premium_pct", DEFAULT_MIN_PREMIUM_PCT);
    }
    if (!config.params.has("max_position_xrp")) {
        config_.params.set("max_position_xrp", DEFAULT_MAX_POSITION_XRP);
    }
    if (!config.params.has("fee_pct")) {
        config_.params.set("fee_pct", DEFAULT_FEE_PCT);
    }
    if (!config.params.has("min_order_qty")) {
        config_.params.set("min_order_qty", DEFAULT_MIN_ORDER_QTY);
    }
}

// =============================================================================
// 기회 평가
// =============================================================================
StrategyDecision BasicArbStrategy::evaluate(const MarketSnapshot& snapshot) {
    // 실행 중이 아니면 스킵
    if (state_ != StrategyState::Running) {
        return StrategyDecision::no_action("Strategy not running");
    }

    // 1. 최고 김프 기회 탐색
    Opportunity opp = find_best_opportunity(snapshot);
    if (!opp.valid) {
        return StrategyDecision::no_action("No valid opportunity");
    }

    // 2. 최소 프리미엄 체크
    double min_premium = config_.params.get("min_premium_pct", DEFAULT_MIN_PREMIUM_PCT);
    if (opp.premium_pct < min_premium) {
        return StrategyDecision::no_action("Premium too low");
    }

    // 3. 수수료 차감 후 순수익 체크
    double fee_pct = config_.params.get("fee_pct", DEFAULT_FEE_PCT);
    if (opp.premium_pct <= fee_pct) {
        return StrategyDecision::no_action("Not profitable after fees");
    }

    // 4. 주문 수량 계산
    double qty = calculate_order_qty(snapshot, opp);
    double min_qty = config_.params.get("min_order_qty", DEFAULT_MIN_ORDER_QTY);
    if (qty < min_qty) {
        return StrategyDecision::no_action("Order quantity too small");
    }

    // 5. 예상 수익 계산
    double expected_profit = calculate_expected_profit(qty, opp, fee_pct);
    double expected_profit_pct = opp.premium_pct - fee_pct;

    // 6. 주문 요청 생성
    DualOrderRequest request = create_order_request(opp, qty);

    // 7. 신뢰도 계산 (프리미엄 기반)
    double confidence = std::min(opp.premium_pct / (min_premium * 2.0), 1.0);

    // 8. 결정 반환
    return StrategyDecision::execute(
        request,
        confidence,
        expected_profit,
        expected_profit_pct,
        "Opportunity found"
    );
}

// =============================================================================
// 최고 김프 기회 탐색
// =============================================================================
BasicArbStrategy::Opportunity BasicArbStrategy::find_best_opportunity(
    const MarketSnapshot& snapshot
) const {
    Opportunity best;
    best.valid = false;
    best.premium_pct = -100.0;

    // 해외 거래소에서 매수, 국내 거래소에서 매도
    Exchange overseas[] = {Exchange::Binance, Exchange::MEXC};
    Exchange domestic[] = {Exchange::Upbit, Exchange::Bithumb};

    for (Exchange buy_ex : overseas) {
        if (!snapshot.has_ticker(buy_ex)) continue;

        for (Exchange sell_ex : domestic) {
            if (!snapshot.has_ticker(sell_ex)) continue;

            // 김프 계산
            double premium = snapshot.get_premium(buy_ex, sell_ex);

            if (premium > best.premium_pct) {
                best.buy_exchange = buy_ex;
                best.sell_exchange = sell_ex;
                best.premium_pct = premium;
                best.buy_price = snapshot.get_ticker(buy_ex).price;
                best.sell_price = snapshot.get_ticker(sell_ex).price;
                best.valid = true;
            }
        }
    }

    return best;
}

// =============================================================================
// 주문 수량 계산
// =============================================================================
double BasicArbStrategy::calculate_order_qty(
    const MarketSnapshot& snapshot,
    const Opportunity& opp
) const {
    double max_position = config_.params.get("max_position_xrp", DEFAULT_MAX_POSITION_XRP);

    // 최대 포지션 금액 제한
    double max_position_krw = config_.max_position_krw;
    double max_qty_by_krw = max_position_krw / opp.sell_price;

    // 설정된 최대 수량
    double qty = std::min(max_position, max_qty_by_krw);

    // 최소 수량 이상인지 확인
    double min_qty = config_.params.get("min_order_qty", DEFAULT_MIN_ORDER_QTY);
    if (qty < min_qty) {
        return 0.0;
    }

    return qty;
}

// =============================================================================
// 예상 수익 계산
// =============================================================================
double BasicArbStrategy::calculate_expected_profit(
    double qty,
    const Opportunity& opp,
    double fee_pct
) const {
    double position_krw = qty * opp.sell_price;
    double net_premium_pct = opp.premium_pct - fee_pct;
    return position_krw * net_premium_pct / 100.0;
}

// =============================================================================
// 주문 요청 생성
// =============================================================================
DualOrderRequest BasicArbStrategy::create_order_request(
    const Opportunity& opp,
    double qty
) const {
    DualOrderRequest request;
    request.set_request_id_auto();
    request.expected_premium = opp.premium_pct;

    // 매수 주문 (해외)
    request.buy_order.exchange = opp.buy_exchange;
    request.buy_order.set_symbol("XRP");
    request.buy_order.side = OrderSide::Buy;
    request.buy_order.type = OrderType::Market;  // Taker
    request.buy_order.quantity = qty;
    request.buy_order.price = opp.buy_price;

    // 매도 주문 (국내)
    request.sell_order.exchange = opp.sell_exchange;
    request.sell_order.set_symbol("XRP");
    request.sell_order.side = OrderSide::Sell;
    request.sell_order.type = OrderType::Market;  // Taker
    request.sell_order.quantity = qty;
    request.sell_order.price = opp.sell_price;

    return request;
}

}  // namespace arbitrage
