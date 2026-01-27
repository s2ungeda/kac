#pragma once

#include "arbitrage/common/types.hpp"
#include <cstring>
#include <chrono>

namespace arbitrage {

// =============================================================================
// Depth Level (호가별 상세 정보)
// =============================================================================
struct DepthLevel {
    double price{0.0};             // 호가 가격
    double quantity{0.0};          // 해당 호가 수량
    double cumulative_qty{0.0};    // 누적 수량
    double cumulative_value{0.0};  // 누적 금액 (price * quantity 합계)
    double vwap{0.0};              // 여기까지 VWAP (Volume Weighted Avg Price)
    double distance_pct{0.0};      // 최우선가 대비 거리(%)
    int level{0};                  // 호가 레벨 (0부터)

    void calculate_vwap() {
        if (cumulative_qty > 0.0) {
            vwap = cumulative_value / cumulative_qty;
        }
    }
};

// =============================================================================
// Liquidity Metrics (유동성 측정 결과)
// Cache-line aligned for low-latency
// =============================================================================
struct alignas(CACHE_LINE_SIZE) LiquidityMetrics {
    Exchange exchange{Exchange::Upbit};
    char symbol[MAX_SYMBOL_LEN]{};
    uint8_t _pad1[7]{};

    // 스프레드
    double best_bid{0.0};           // 최우선 매수가
    double best_ask{0.0};           // 최우선 매도가
    double spread{0.0};             // 스프레드 (절대값)
    double spread_bps{0.0};         // 스프레드 (basis points, 0.01%)

    // 깊이 (1% 범위 내 물량)
    double bid_depth_1pct{0.0};     // 매수 깊이 (1% 범위, 수량)
    double ask_depth_1pct{0.0};     // 매도 깊이 (1% 범위, 수량)
    double bid_value_1pct{0.0};     // 매수 깊이 (1% 범위, 금액)
    double ask_value_1pct{0.0};     // 매도 깊이 (1% 범위, 금액)

    // 불균형
    double imbalance{0.0};          // -1(매도벽) ~ +1(매수벽)

    // 레벨별 상세
    int bid_levels{0};              // 분석된 매수 레벨 수
    int ask_levels{0};              // 분석된 매도 레벨 수

    // 시간
    int64_t timestamp_us{0};

    // 헬퍼 함수
    void set_symbol(const char* s) {
        std::strncpy(symbol, s, MAX_SYMBOL_LEN - 1);
        symbol[MAX_SYMBOL_LEN - 1] = '\0';
    }

    void set_symbol(const std::string& s) {
        set_symbol(s.c_str());
    }

    void set_timestamp_now() {
        auto now = std::chrono::system_clock::now();
        timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
    }

    // 중간가
    double mid_price() const {
        return (best_bid + best_ask) / 2.0;
    }

    // 유효한 메트릭인지
    bool is_valid() const {
        return best_bid > 0.0 && best_ask > 0.0 && best_bid < best_ask;
    }

    // 유동성 충분 여부 (금액 기준)
    bool has_sufficient_liquidity(double min_value) const {
        return bid_value_1pct >= min_value && ask_value_1pct >= min_value;
    }
};

// =============================================================================
// Liquidity Calculator
// =============================================================================
class LiquidityCalculator {
public:
    // 설정값
    void set_depth_range_pct(double pct) { depth_range_pct_ = pct; }
    double depth_range_pct() const { return depth_range_pct_; }

    // OrderBook에서 유동성 메트릭 계산
    LiquidityMetrics calculate(const OrderBook& ob) const;

    // 특정 수량까지의 깊이 분석
    // @param ob 오더북
    // @param side Buy면 ask 분석, Sell이면 bid 분석
    // @param quantity 분석할 수량
    // @param levels 결과를 저장할 DepthLevel 배열
    // @param max_levels 배열 크기
    // @return 실제 분석된 레벨 수
    int analyze_depth(
        const OrderBook& ob,
        OrderSide side,
        double quantity,
        DepthLevel* levels,
        int max_levels
    ) const;

    // 불균형 계산
    static double calculate_imbalance(double bid_depth, double ask_depth);

private:
    double depth_range_pct_{1.0};  // 1% 범위 기본값
};

// =============================================================================
// 유동성 경고 타입
// =============================================================================
enum class LiquidityAlert : uint8_t {
    None = 0,
    LowBidDepth,      // 매수 유동성 부족
    LowAskDepth,      // 매도 유동성 부족
    WideSpread,       // 스프레드 과다
    HighImbalance,    // 불균형 과다
    RapidChange       // 급격한 변화
};

constexpr const char* liquidity_alert_name(LiquidityAlert alert) {
    switch (alert) {
        case LiquidityAlert::None:         return "None";
        case LiquidityAlert::LowBidDepth:  return "LowBidDepth";
        case LiquidityAlert::LowAskDepth:  return "LowAskDepth";
        case LiquidityAlert::WideSpread:   return "WideSpread";
        case LiquidityAlert::HighImbalance: return "HighImbalance";
        case LiquidityAlert::RapidChange:  return "RapidChange";
        default:                           return "Unknown";
    }
}

}  // namespace arbitrage
