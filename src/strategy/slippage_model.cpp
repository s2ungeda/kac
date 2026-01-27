#include "arbitrage/strategy/slippage_model.hpp"
#include <cmath>
#include <algorithm>

namespace arbitrage {

// =============================================================================
// SlippageModel Implementation
// =============================================================================

SlippageEstimate SlippageModel::estimate_taker_slippage(
    const OrderBook& ob,
    OrderSide side,
    double quantity
) const {
    SlippageEstimate estimate;
    estimate.exchange = ob.exchange;
    estimate.side = side;
    estimate.quantity = quantity;

    if (quantity <= 0.0) return estimate;

    const PriceLevel* levels;
    int level_count;

    if (side == OrderSide::Buy) {
        // 매수 → ask에서 체결 (오름차순)
        levels = ob.asks;
        level_count = ob.ask_count;
        estimate.best_price = ob.best_ask();
    } else {
        // 매도 → bid에서 체결 (내림차순)
        levels = ob.bids;
        level_count = ob.bid_count;
        estimate.best_price = ob.best_bid();
    }

    if (estimate.best_price <= 0.0 || level_count == 0) {
        return estimate;
    }

    double cumulative_qty = 0.0;
    double cumulative_value = 0.0;
    int consumed_levels = 0;

    for (int i = 0; i < level_count && i < MAX_ORDERBOOK_DEPTH; ++i) {
        const auto& pl = levels[i];
        if (pl.price <= 0.0 || pl.quantity <= 0.0) continue;

        double remaining = quantity - cumulative_qty;
        double fill_qty = std::min(pl.quantity, remaining);

        cumulative_qty += fill_qty;
        cumulative_value += pl.price * fill_qty;

        // 체결 경로 기록
        if (estimate.execution_path_count < SlippageEstimate::MAX_EXECUTION_LEVELS) {
            auto& ep = estimate.execution_path[estimate.execution_path_count];
            ep.price = pl.price;
            ep.quantity = fill_qty;
            ep.cumulative_qty = cumulative_qty;
            ep.cumulative_value = cumulative_value;
            ep.calculate_vwap();
            ep.level = estimate.execution_path_count;

            if (side == OrderSide::Buy) {
                ep.distance_pct = ((pl.price - estimate.best_price) / estimate.best_price) * 100.0;
            } else {
                ep.distance_pct = ((estimate.best_price - pl.price) / estimate.best_price) * 100.0;
            }

            ++estimate.execution_path_count;
        }

        estimate.worst_price = pl.price;
        ++consumed_levels;

        if (cumulative_qty >= quantity) break;
    }

    estimate.fillable_qty = cumulative_qty;
    estimate.fill_ratio = (quantity > 0.0) ? std::min(cumulative_qty / quantity, 1.0) : 0.0;
    estimate.fully_fillable = (cumulative_qty >= quantity);
    estimate.levels_consumed = consumed_levels;

    // 평균 체결가
    if (cumulative_qty > 0.0) {
        estimate.expected_avg_price = cumulative_value / cumulative_qty;
    }

    // 슬리피지 계산
    if (estimate.best_price > 0.0 && estimate.expected_avg_price > 0.0) {
        double diff;
        if (side == OrderSide::Buy) {
            // 매수: 평균가 > 최우선가 → 양의 슬리피지
            diff = estimate.expected_avg_price - estimate.best_price;
        } else {
            // 매도: 평균가 < 최우선가 → 양의 슬리피지
            diff = estimate.best_price - estimate.expected_avg_price;
        }
        estimate.slippage_bps = (diff / estimate.best_price) * 10000.0;
        estimate.slippage_value = diff * cumulative_qty;
    }

    return estimate;
}

SlippageEstimate SlippageModel::estimate_slippage_to_price(
    const OrderBook& ob,
    OrderSide side,
    double limit_price
) const {
    SlippageEstimate estimate;
    estimate.exchange = ob.exchange;
    estimate.side = side;

    if (limit_price <= 0.0) return estimate;

    const PriceLevel* levels;
    int level_count;

    if (side == OrderSide::Buy) {
        levels = ob.asks;
        level_count = ob.ask_count;
        estimate.best_price = ob.best_ask();
    } else {
        levels = ob.bids;
        level_count = ob.bid_count;
        estimate.best_price = ob.best_bid();
    }

    if (estimate.best_price <= 0.0 || level_count == 0) {
        return estimate;
    }

    double cumulative_qty = 0.0;
    double cumulative_value = 0.0;

    for (int i = 0; i < level_count && i < MAX_ORDERBOOK_DEPTH; ++i) {
        const auto& pl = levels[i];
        if (pl.price <= 0.0 || pl.quantity <= 0.0) continue;

        // 가격 범위 체크
        if (side == OrderSide::Buy && pl.price > limit_price) break;
        if (side == OrderSide::Sell && pl.price < limit_price) break;

        cumulative_qty += pl.quantity;
        cumulative_value += pl.price * pl.quantity;
        estimate.worst_price = pl.price;
        ++estimate.levels_consumed;
    }

    estimate.quantity = cumulative_qty;
    estimate.fillable_qty = cumulative_qty;
    estimate.fill_ratio = 1.0;
    estimate.fully_fillable = true;

    if (cumulative_qty > 0.0) {
        estimate.expected_avg_price = cumulative_value / cumulative_qty;
    }

    // 슬리피지 계산
    if (estimate.best_price > 0.0 && estimate.expected_avg_price > 0.0) {
        double diff;
        if (side == OrderSide::Buy) {
            diff = estimate.expected_avg_price - estimate.best_price;
        } else {
            diff = estimate.best_price - estimate.expected_avg_price;
        }
        estimate.slippage_bps = (diff / estimate.best_price) * 10000.0;
        estimate.slippage_value = diff * cumulative_qty;
    }

    return estimate;
}

MakerPriceEstimate SlippageModel::calculate_optimal_maker_price(
    const OrderBook& ob,
    OrderSide side,
    double target_fill_prob,
    double max_wait_sec
) const {
    MakerPriceEstimate estimate;
    estimate.exchange = ob.exchange;
    estimate.side = side;

    if (side == OrderSide::Buy) {
        // 매수 Maker → bid 쪽에 주문
        estimate.best_price = ob.best_bid();
    } else {
        // 매도 Maker → ask 쪽에 주문
        estimate.best_price = ob.best_ask();
    }

    if (estimate.best_price <= 0.0) {
        return estimate;
    }

    // 간단한 휴리스틱 모델:
    // - 체결 확률은 최우선가에서 멀어질수록 감소
    // - 대기 시간은 최우선가에서 멀어질수록 증가

    // 목표 체결 확률에 따른 가격 오프셋 계산
    // 확률이 높을수록 최우선가에 가깝게
    // 확률이 낮을수록 더 유리한 가격에 (체결 불확실)

    // 스프레드 기반 조정
    double spread = ob.spread();
    double mid = ob.mid_price();

    if (mid <= 0.0) {
        estimate.recommended_price = estimate.best_price;
        estimate.estimated_fill_prob = 0.5;
        return estimate;
    }

    // 목표 확률에 따른 가격 조정
    // 80% 확률 → 최우선가 근처
    // 50% 확률 → 스프레드 중간
    // 20% 확률 → 상대방 최우선가 근처

    double prob_factor = target_fill_prob;  // 0~1
    double price_offset = spread * (1.0 - prob_factor);

    if (side == OrderSide::Buy) {
        // 매수: 낮은 가격일수록 유리하지만 체결 확률 낮음
        estimate.recommended_price = estimate.best_price - price_offset;
        estimate.distance_from_best_bps = (price_offset / estimate.best_price) * 10000.0;
    } else {
        // 매도: 높은 가격일수록 유리하지만 체결 확률 낮음
        estimate.recommended_price = estimate.best_price + price_offset;
        estimate.distance_from_best_bps = (price_offset / estimate.best_price) * 10000.0;
    }

    estimate.estimated_fill_prob = target_fill_prob;

    // 대기 시간 추정 (레벨당 fill_time_per_level_ 초)
    int estimated_levels = static_cast<int>(estimate.distance_from_best_bps / 10.0);  // 10 bps per level approx
    estimate.estimated_wait_sec = std::min(
        static_cast<double>(estimated_levels) * fill_time_per_level_,
        max_wait_sec
    );

    return estimate;
}

// =============================================================================
// VWAP Utility Functions
// =============================================================================

namespace vwap {

double calculate_vwap(
    const PriceLevel* levels,
    int level_count,
    double quantity
) {
    if (quantity <= 0.0 || level_count == 0) return 0.0;

    double cumulative_qty = 0.0;
    double cumulative_value = 0.0;

    for (int i = 0; i < level_count; ++i) {
        const auto& pl = levels[i];
        if (pl.price <= 0.0 || pl.quantity <= 0.0) continue;

        double remaining = quantity - cumulative_qty;
        double fill_qty = std::min(pl.quantity, remaining);

        cumulative_qty += fill_qty;
        cumulative_value += pl.price * fill_qty;

        if (cumulative_qty >= quantity) break;
    }

    return (cumulative_qty > 0.0) ? (cumulative_value / cumulative_qty) : 0.0;
}

double calculate_avg_price_for_value(
    const PriceLevel* levels,
    int level_count,
    double target_value
) {
    if (target_value <= 0.0 || level_count == 0) return 0.0;

    double cumulative_qty = 0.0;
    double cumulative_value = 0.0;

    for (int i = 0; i < level_count; ++i) {
        const auto& pl = levels[i];
        if (pl.price <= 0.0 || pl.quantity <= 0.0) continue;

        double level_value = pl.price * pl.quantity;
        double remaining_value = target_value - cumulative_value;

        if (level_value <= remaining_value) {
            cumulative_qty += pl.quantity;
            cumulative_value += level_value;
        } else {
            // 부분 체결
            double partial_qty = remaining_value / pl.price;
            cumulative_qty += partial_qty;
            cumulative_value += remaining_value;
            break;
        }
    }

    return (cumulative_qty > 0.0) ? (cumulative_value / cumulative_qty) : 0.0;
}

}  // namespace vwap

}  // namespace arbitrage
