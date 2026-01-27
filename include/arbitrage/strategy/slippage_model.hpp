#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/strategy/liquidity_metrics.hpp"
#include <vector>
#include <array>

namespace arbitrage {

// =============================================================================
// Slippage Estimate (슬리피지 예측 결과)
// =============================================================================
struct SlippageEstimate {
    Exchange exchange{Exchange::Upbit};
    OrderSide side{OrderSide::Buy};
    double quantity{0.0};               // 주문 수량

    double best_price{0.0};             // 최우선가
    double expected_avg_price{0.0};     // 예상 평균 체결가
    double worst_price{0.0};            // 최악 체결가 (마지막 레벨)
    double slippage_bps{0.0};           // 슬리피지 (bps, 0.01%)
    double slippage_value{0.0};         // 슬리피지 금액

    int levels_consumed{0};             // 소진되는 호가 레벨 수
    double fillable_qty{0.0};           // 체결 가능 수량
    double fill_ratio{0.0};             // 체결 가능 비율 (0~1)
    bool fully_fillable{false};         // 전량 체결 가능 여부

    // 체결 경로 (레벨별 상세)
    static constexpr int MAX_EXECUTION_LEVELS = MAX_ORDERBOOK_DEPTH;
    DepthLevel execution_path[MAX_EXECUTION_LEVELS];
    int execution_path_count{0};

    // 유효성 검사
    bool is_valid() const {
        return quantity > 0.0 && best_price > 0.0;
    }

    // 슬리피지 퍼센트 (편의 함수)
    double slippage_pct() const {
        return slippage_bps / 100.0;  // bps to percent
    }
};

// =============================================================================
// Maker Price Estimate (Maker 최적 가격 추정)
// =============================================================================
struct MakerPriceEstimate {
    Exchange exchange{Exchange::Upbit};
    OrderSide side{OrderSide::Buy};

    double best_price{0.0};             // 현재 최우선가
    double recommended_price{0.0};      // 추천 Maker 가격
    double distance_from_best_bps{0.0}; // 최우선가 대비 거리 (bps)

    double estimated_fill_prob{0.0};    // 예상 체결 확률
    double estimated_wait_sec{0.0};     // 예상 대기 시간 (초)

    bool is_valid() const {
        return best_price > 0.0 && recommended_price > 0.0;
    }
};

// =============================================================================
// Slippage Model (슬리피지 모델)
// =============================================================================
class SlippageModel {
public:
    // ==========================================================================
    // Taker 슬리피지 예측
    // ==========================================================================

    // 시장가 주문 슬리피지 예측
    // @param ob 오더북
    // @param side Buy면 ask에서 체결, Sell이면 bid에서 체결
    // @param quantity 주문 수량
    // @return 슬리피지 예측 결과
    SlippageEstimate estimate_taker_slippage(
        const OrderBook& ob,
        OrderSide side,
        double quantity
    ) const;

    // 특정 가격까지의 슬리피지 예측
    SlippageEstimate estimate_slippage_to_price(
        const OrderBook& ob,
        OrderSide side,
        double limit_price
    ) const;

    // ==========================================================================
    // Maker 가격 산출
    // ==========================================================================

    // Maker 최적 가격 계산
    // @param ob 오더북
    // @param side Buy면 bid 쪽에 주문, Sell이면 ask 쪽에 주문
    // @param target_fill_prob 목표 체결 확률 (0~1)
    // @param max_wait_sec 최대 대기 시간 (초)
    // @return 추천 Maker 가격 정보
    MakerPriceEstimate calculate_optimal_maker_price(
        const OrderBook& ob,
        OrderSide side,
        double target_fill_prob = 0.8,
        double max_wait_sec = 30.0
    ) const;

    // ==========================================================================
    // 설정
    // ==========================================================================

    // 기본 체결 시간 추정값 (레벨당 초)
    void set_fill_time_per_level(double sec) { fill_time_per_level_ = sec; }
    double fill_time_per_level() const { return fill_time_per_level_; }

    // 스프레드 기반 체결 확률 감소율
    void set_prob_decay_per_bps(double decay) { prob_decay_per_bps_ = decay; }
    double prob_decay_per_bps() const { return prob_decay_per_bps_; }

private:
    // 기본 설정값
    double fill_time_per_level_{1.0};   // 레벨당 1초 추정
    double prob_decay_per_bps_{0.01};   // bps당 1% 확률 감소
};

// =============================================================================
// VWAP Calculator (편의 함수)
// =============================================================================
namespace vwap {

// 특정 수량 체결 시 VWAP 계산
double calculate_vwap(
    const PriceLevel* levels,
    int level_count,
    double quantity
);

// 특정 금액 체결 시 평균 가격 계산
double calculate_avg_price_for_value(
    const PriceLevel* levels,
    int level_count,
    double target_value
);

}  // namespace vwap

}  // namespace arbitrage
