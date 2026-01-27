#include "arbitrage/strategy/orderbook_analyzer.hpp"
#include <cmath>
#include <algorithm>

namespace arbitrage {

// =============================================================================
// OrderBookAnalyzer Implementation
// =============================================================================

OrderBookAnalyzer::OrderBookAnalyzer(FeeCalculator* fee_calc)
    : fee_calc_(fee_calc) {
    // 오더북 초기화
    for (int i = 0; i < static_cast<int>(Exchange::Count); ++i) {
        orderbooks_[i].exchange = static_cast<Exchange>(i);
        orderbooks_[i].clear();
    }
}

void OrderBookAnalyzer::update(Exchange ex, const OrderBook& ob) {
    int idx = static_cast<int>(ex);
    if (idx < 0 || idx >= static_cast<int>(Exchange::Count)) return;

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        orderbooks_[idx] = ob;
        metrics_[idx] = liquidity_calc_.calculate(ob);
    }

    stats_.updates.fetch_add(1, std::memory_order_relaxed);

    // 유동성 경고 체크
    check_liquidity_alerts(ex, metrics_[idx]);
}

bool OrderBookAnalyzer::get_orderbook(Exchange ex, OrderBook& out) const {
    int idx = static_cast<int>(ex);
    if (idx < 0 || idx >= static_cast<int>(Exchange::Count)) return false;

    std::shared_lock<std::shared_mutex> lock(mutex_);
    out = orderbooks_[idx];
    stats_.queries.fetch_add(1, std::memory_order_relaxed);
    return out.bid_count > 0 || out.ask_count > 0;
}

LiquidityMetrics OrderBookAnalyzer::get_liquidity(Exchange ex) const {
    int idx = static_cast<int>(ex);
    if (idx < 0 || idx >= static_cast<int>(Exchange::Count)) {
        return LiquidityMetrics{};
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    stats_.queries.fetch_add(1, std::memory_order_relaxed);
    return metrics_[idx];
}

void OrderBookAnalyzer::get_all_liquidity(LiquidityMetrics* out, int* count) const {
    if (!out || !count) return;

    std::shared_lock<std::shared_mutex> lock(mutex_);
    int n = static_cast<int>(Exchange::Count);
    for (int i = 0; i < n; ++i) {
        out[i] = metrics_[i];
    }
    *count = n;
    stats_.queries.fetch_add(1, std::memory_order_relaxed);
}

SlippageEstimate OrderBookAnalyzer::estimate_slippage(
    Exchange ex,
    OrderSide side,
    double quantity
) const {
    int idx = static_cast<int>(ex);
    if (idx < 0 || idx >= static_cast<int>(Exchange::Count)) {
        return SlippageEstimate{};
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    stats_.queries.fetch_add(1, std::memory_order_relaxed);
    return slippage_model_.estimate_taker_slippage(orderbooks_[idx], side, quantity);
}

DualOrderPlan OrderBookAnalyzer::plan_maker_taker_order(
    Exchange buy_ex,
    Exchange sell_ex,
    double quantity,
    double fx_rate
) const {
    DualOrderPlan plan;

    if (quantity <= 0.0) return plan;

    int buy_idx = static_cast<int>(buy_ex);
    int sell_idx = static_cast<int>(sell_ex);
    if (buy_idx < 0 || buy_idx >= static_cast<int>(Exchange::Count)) return plan;
    if (sell_idx < 0 || sell_idx >= static_cast<int>(Exchange::Count)) return plan;

    std::shared_lock<std::shared_mutex> lock(mutex_);

    const auto& buy_ob = orderbooks_[buy_idx];
    const auto& sell_ob = orderbooks_[sell_idx];

    // 일반적 패턴: 해외(매수) = Maker, 국내(매도) = Taker
    // Maker 가격 계산 (해외 거래소)
    auto maker_estimate = slippage_model_.calculate_optimal_maker_price(
        buy_ob,
        OrderSide::Buy,
        config_.maker_fill_probability,
        config_.maker_max_wait_sec
    );

    // Taker 슬리피지 계산 (국내 거래소)
    auto taker_estimate = slippage_model_.estimate_taker_slippage(
        sell_ob,
        OrderSide::Sell,
        quantity
    );

    // Maker 측 정보
    plan.maker_exchange = buy_ex;
    plan.maker_side = OrderSide::Buy;
    plan.maker_price = maker_estimate.recommended_price;
    plan.maker_quantity = quantity;
    plan.maker_fee_rate = get_fee_rate(buy_ex, true);  // Maker
    plan.expected_fill_time_sec = maker_estimate.estimated_wait_sec;

    // Taker 측 정보
    plan.taker_exchange = sell_ex;
    plan.taker_side = OrderSide::Sell;
    plan.taker_price = taker_estimate.expected_avg_price;
    plan.taker_quantity = quantity;
    plan.taker_fee_rate = get_fee_rate(sell_ex, false);  // Taker
    plan.taker_slippage_bps = taker_estimate.slippage_bps;
    plan.taker_slippage_value = taker_estimate.slippage_value;

    if (!plan.is_valid()) return plan;

    // 비용 계산
    // 해외 매수가 (KRW 환산)
    double buy_price_krw = plan.maker_price * fx_rate;
    double buy_value_krw = buy_price_krw * quantity;

    // 국내 매도가 (이미 KRW)
    double sell_price_krw = plan.taker_price;
    double sell_value_krw = sell_price_krw * quantity;

    // 수수료 계산
    double maker_fee_value = buy_value_krw * plan.maker_fee_rate;
    double taker_fee_value = sell_value_krw * plan.taker_fee_rate;
    plan.total_fee_value = maker_fee_value + taker_fee_value;

    // 슬리피지 (Taker만)
    plan.total_slippage_value = plan.taker_slippage_value;

    // 프리미엄 계산
    // 김프 = (국내가 - 해외가) / 해외가 * 100
    if (buy_price_krw > 0.0) {
        plan.gross_premium_pct = ((sell_price_krw - buy_price_krw) / buy_price_krw) * 100.0;

        // 총 비용 (수수료 + 슬리피지) 퍼센트
        double total_cost_pct = ((plan.total_fee_value + plan.total_slippage_value) / buy_value_krw) * 100.0;

        plan.net_premium_pct = plan.gross_premium_pct - total_cost_pct;

        // 예상 순이익
        plan.expected_profit_value = sell_value_krw - buy_value_krw - plan.total_fee_value - plan.total_slippage_value;
    }

    stats_.queries.fetch_add(1, std::memory_order_relaxed);

    return plan;
}

double OrderBookAnalyzer::calculate_breakeven_premium(
    Exchange buy_ex,
    Exchange sell_ex
) const {
    // 손익분기 프리미엄 = 매수 수수료 + 매도 수수료 + 예상 슬리피지
    double maker_fee = get_fee_rate(buy_ex, true) * 100.0;   // % 변환
    double taker_fee = get_fee_rate(sell_ex, false) * 100.0; // % 변환

    // 예상 슬리피지 (보수적 추정)
    double estimated_slippage_pct = 0.1;  // 0.1% 기본값

    return maker_fee + taker_fee + estimated_slippage_pct;
}

void OrderBookAnalyzer::set_alert_callback(LiquidityAlertCallback cb) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    alert_callback_ = std::move(cb);
}

void OrderBookAnalyzer::set_config(const OrderBookAnalyzerConfig& config) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    config_ = config;
}

void OrderBookAnalyzer::check_liquidity_alerts(Exchange ex, const LiquidityMetrics& metrics) {
    if (!alert_callback_) return;

    LiquidityAlert alert = LiquidityAlert::None;
    const char* message = "";

    // 매수 유동성 부족
    if (metrics.bid_value_1pct < config_.min_depth_value) {
        alert = LiquidityAlert::LowBidDepth;
        message = "Low bid depth";
    }
    // 매도 유동성 부족
    else if (metrics.ask_value_1pct < config_.min_depth_value) {
        alert = LiquidityAlert::LowAskDepth;
        message = "Low ask depth";
    }
    // 스프레드 과다
    else if (metrics.spread_bps > config_.max_spread_bps) {
        alert = LiquidityAlert::WideSpread;
        message = "Wide spread";
    }
    // 불균형 과다 (절대값 0.7 이상)
    else if (std::abs(metrics.imbalance) > 0.7) {
        alert = LiquidityAlert::HighImbalance;
        message = "High imbalance";
    }

    if (alert != LiquidityAlert::None) {
        stats_.alerts.fetch_add(1, std::memory_order_relaxed);
        alert_callback_(ex, alert, message);
    }
}

double OrderBookAnalyzer::get_fee_rate(Exchange ex, bool is_maker) const {
    // FeeCalculator가 있으면 사용, 없으면 기본값
    // (TASK_11에서 FeeCalculator 구현 후 연동)

    if (is_maker) {
        // Maker 수수료
        switch (ex) {
            case Exchange::Binance: return 0.00075;  // 0.075% (BNB 할인 적용)
            case Exchange::MEXC:    return 0.0;      // 0% (Maker 무료)
            default:                return config_.default_maker_fee;
        }
    } else {
        // Taker 수수료
        switch (ex) {
            case Exchange::Upbit:   return 0.0005;   // 0.05%
            case Exchange::Bithumb: return 0.0004;   // 0.04%
            case Exchange::Binance: return 0.00075;  // 0.075% (BNB 할인 적용)
            case Exchange::MEXC:    return 0.001;    // 0.1%
            default:                return config_.default_taker_fee;
        }
    }
}

}  // namespace arbitrage
