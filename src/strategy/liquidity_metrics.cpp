#include "arbitrage/strategy/liquidity_metrics.hpp"
#include <cmath>
#include <algorithm>

namespace arbitrage {

// =============================================================================
// LiquidityCalculator Implementation
// =============================================================================

LiquidityMetrics LiquidityCalculator::calculate(const OrderBook& ob) const {
    LiquidityMetrics metrics;

    metrics.exchange = ob.exchange;
    std::strncpy(metrics.symbol, ob.symbol, MAX_SYMBOL_LEN - 1);
    metrics.symbol[MAX_SYMBOL_LEN - 1] = '\0';

    // 최우선가
    metrics.best_bid = ob.best_bid();
    metrics.best_ask = ob.best_ask();

    if (metrics.best_bid <= 0.0 || metrics.best_ask <= 0.0) {
        return metrics;  // 유효하지 않은 오더북
    }

    // 스프레드 계산
    metrics.spread = metrics.best_ask - metrics.best_bid;
    double mid = metrics.mid_price();
    if (mid > 0.0) {
        metrics.spread_bps = (metrics.spread / mid) * 10000.0;  // bps = 0.01%
    }

    // 1% 범위 가격 계산
    double bid_threshold = metrics.best_bid * (1.0 - depth_range_pct_ / 100.0);
    double ask_threshold = metrics.best_ask * (1.0 + depth_range_pct_ / 100.0);

    // 매수 깊이 계산 (1% 범위)
    double bid_qty = 0.0;
    double bid_value = 0.0;
    int bid_levels = 0;

    for (int i = 0; i < ob.bid_count && i < MAX_ORDERBOOK_DEPTH; ++i) {
        if (ob.bids[i].price < bid_threshold) break;
        bid_qty += ob.bids[i].quantity;
        bid_value += ob.bids[i].price * ob.bids[i].quantity;
        ++bid_levels;
    }

    metrics.bid_depth_1pct = bid_qty;
    metrics.bid_value_1pct = bid_value;
    metrics.bid_levels = bid_levels;

    // 매도 깊이 계산 (1% 범위)
    double ask_qty = 0.0;
    double ask_value = 0.0;
    int ask_levels = 0;

    for (int i = 0; i < ob.ask_count && i < MAX_ORDERBOOK_DEPTH; ++i) {
        if (ob.asks[i].price > ask_threshold) break;
        ask_qty += ob.asks[i].quantity;
        ask_value += ob.asks[i].price * ob.asks[i].quantity;
        ++ask_levels;
    }

    metrics.ask_depth_1pct = ask_qty;
    metrics.ask_value_1pct = ask_value;
    metrics.ask_levels = ask_levels;

    // 불균형 계산
    metrics.imbalance = calculate_imbalance(bid_qty, ask_qty);

    // 타임스탬프
    metrics.set_timestamp_now();

    return metrics;
}

int LiquidityCalculator::analyze_depth(
    const OrderBook& ob,
    OrderSide side,
    double target_quantity,
    DepthLevel* levels,
    int max_levels
) const {
    const PriceLevel* source;
    int source_count;
    double best_price;

    if (side == OrderSide::Buy) {
        // 매수 → ask에서 체결
        source = ob.asks;
        source_count = ob.ask_count;
        best_price = ob.best_ask();
    } else {
        // 매도 → bid에서 체결
        source = ob.bids;
        source_count = ob.bid_count;
        best_price = ob.best_bid();
    }

    if (best_price <= 0.0) return 0;

    double cumulative_qty = 0.0;
    double cumulative_value = 0.0;
    int level_count = 0;

    for (int i = 0; i < source_count && i < max_levels && level_count < max_levels; ++i) {
        const auto& pl = source[i];
        if (pl.price <= 0.0 || pl.quantity <= 0.0) continue;

        // 남은 수량 계산
        double remaining = target_quantity - cumulative_qty;
        double fill_qty = std::min(pl.quantity, remaining);

        cumulative_qty += fill_qty;
        cumulative_value += pl.price * fill_qty;

        // DepthLevel 설정
        auto& dl = levels[level_count];
        dl.price = pl.price;
        dl.quantity = fill_qty;
        dl.cumulative_qty = cumulative_qty;
        dl.cumulative_value = cumulative_value;
        dl.calculate_vwap();
        dl.level = level_count;

        // 최우선가 대비 거리 계산
        if (side == OrderSide::Buy) {
            dl.distance_pct = ((pl.price - best_price) / best_price) * 100.0;
        } else {
            dl.distance_pct = ((best_price - pl.price) / best_price) * 100.0;
        }

        ++level_count;

        // 목표 수량 도달
        if (cumulative_qty >= target_quantity) break;
    }

    return level_count;
}

double LiquidityCalculator::calculate_imbalance(double bid_depth, double ask_depth) {
    double total = bid_depth + ask_depth;
    if (total <= 0.0) return 0.0;

    // -1 (매도 우세) ~ +1 (매수 우세)
    return (bid_depth - ask_depth) / total;
}

}  // namespace arbitrage
