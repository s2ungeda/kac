#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/strategy/liquidity_metrics.hpp"
#include "arbitrage/strategy/slippage_model.hpp"
#include <map>
#include <functional>
#include <shared_mutex>
#include <atomic>

namespace arbitrage {

// 전방 선언 (TASK_11에서 구현 예정)
class FeeCalculator;

// =============================================================================
// Dual Order Plan (Maker+Taker 최적 주문 계획)
// =============================================================================
struct DualOrderPlan {
    // Maker 측 (보통 해외 거래소)
    Exchange maker_exchange{Exchange::Binance};
    OrderSide maker_side{OrderSide::Buy};
    double maker_price{0.0};
    double maker_quantity{0.0};
    double maker_fee_rate{0.001};       // 0.1% 기본값
    double expected_fill_time_sec{0.0}; // 예상 체결 시간

    // Taker 측 (보통 국내 거래소)
    Exchange taker_exchange{Exchange::Upbit};
    OrderSide taker_side{OrderSide::Sell};
    double taker_price{0.0};            // 예상 평균 체결가
    double taker_quantity{0.0};
    double taker_fee_rate{0.0005};      // 0.05% 기본값
    double taker_slippage_bps{0.0};     // 슬리피지 (bps)
    double taker_slippage_value{0.0};   // 슬리피지 금액

    // 비용 계산
    double total_fee_value{0.0};        // 총 수수료 금액
    double total_slippage_value{0.0};   // 총 슬리피지 금액
    double gross_premium_pct{0.0};      // 세전 프리미엄 (%)
    double net_premium_pct{0.0};        // 수수료+슬리피지 차감 후 순 프리미엄 (%)
    double expected_profit_value{0.0};  // 예상 순이익 금액

    bool is_valid() const {
        return maker_price > 0.0 && taker_price > 0.0 && maker_quantity > 0.0;
    }

    bool is_profitable() const {
        return is_valid() && net_premium_pct > 0.0;
    }
};

// =============================================================================
// Analyzer Config
// =============================================================================
struct OrderBookAnalyzerConfig {
    // 유동성 경고 임계값
    double min_depth_value{50000000.0};  // 최소 유동성 금액 (5천만원)
    double max_spread_bps{30.0};          // 최대 허용 스프레드 (0.3%)

    // Maker 가격 산출
    double maker_fill_probability{0.8};   // 80% 체결 확률 목표
    double maker_max_wait_sec{30.0};      // 최대 대기 시간

    // 기본 수수료율 (FeeCalculator 없을 때 사용)
    double default_maker_fee{0.001};      // 0.1%
    double default_taker_fee{0.001};      // 0.1%
};

// =============================================================================
// OrderBook Analyzer
// =============================================================================
class OrderBookAnalyzer {
public:
    // 생성자
    // @param fee_calc 수수료 계산기 (nullptr이면 기본 수수료 사용)
    explicit OrderBookAnalyzer(FeeCalculator* fee_calc = nullptr);

    // 복사/이동 금지
    OrderBookAnalyzer(const OrderBookAnalyzer&) = delete;
    OrderBookAnalyzer& operator=(const OrderBookAnalyzer&) = delete;

    // ==========================================================================
    // 오더북 업데이트
    // ==========================================================================

    // 오더북 업데이트
    void update(Exchange ex, const OrderBook& ob);

    // 모든 오더북 조회
    bool get_orderbook(Exchange ex, OrderBook& out) const;

    // ==========================================================================
    // 유동성 메트릭
    // ==========================================================================

    // 단일 거래소 유동성 조회
    LiquidityMetrics get_liquidity(Exchange ex) const;

    // 모든 거래소 유동성 조회
    void get_all_liquidity(LiquidityMetrics* out, int* count) const;

    // ==========================================================================
    // 슬리피지 예측
    // ==========================================================================

    // Taker 슬리피지 예측
    SlippageEstimate estimate_slippage(
        Exchange ex,
        OrderSide side,
        double quantity
    ) const;

    // ==========================================================================
    // Maker+Taker 주문 계획
    // ==========================================================================

    // Maker+Taker 최적 주문 계획 생성
    // @param buy_ex 매수 거래소 (해외)
    // @param sell_ex 매도 거래소 (국내)
    // @param quantity 거래 수량
    // @param fx_rate USD/KRW 환율
    // @return 최적 주문 계획
    DualOrderPlan plan_maker_taker_order(
        Exchange buy_ex,
        Exchange sell_ex,
        double quantity,
        double fx_rate = 1400.0
    ) const;

    // 손익분기 프리미엄 계산
    // 최소 몇 % 김프여야 수익인가?
    double calculate_breakeven_premium(
        Exchange buy_ex,
        Exchange sell_ex
    ) const;

    // ==========================================================================
    // 유동성 경고
    // ==========================================================================

    using LiquidityAlertCallback = std::function<void(Exchange, LiquidityAlert, const char*)>;

    void set_alert_callback(LiquidityAlertCallback cb);

    // ==========================================================================
    // 설정
    // ==========================================================================

    void set_config(const OrderBookAnalyzerConfig& config);
    const OrderBookAnalyzerConfig& config() const { return config_; }

    void set_min_depth(double value) { config_.min_depth_value = value; }
    void set_max_spread_bps(double bps) { config_.max_spread_bps = bps; }

    // ==========================================================================
    // 통계
    // ==========================================================================

    struct Stats {
        std::atomic<uint64_t> updates{0};
        std::atomic<uint64_t> queries{0};
        std::atomic<uint64_t> alerts{0};

        void reset() {
            updates = 0;
            queries = 0;
            alerts = 0;
        }
    };

    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

private:
    // 유동성 경고 체크
    void check_liquidity_alerts(Exchange ex, const LiquidityMetrics& metrics);

    // 수수료율 가져오기
    double get_fee_rate(Exchange ex, bool is_maker) const;

    // 멤버 변수
    FeeCalculator* fee_calc_;
    OrderBookAnalyzerConfig config_;

    // 거래소별 오더북 (4개)
    OrderBook orderbooks_[static_cast<size_t>(Exchange::Count)];
    LiquidityMetrics metrics_[static_cast<size_t>(Exchange::Count)];

    // 유틸리티 객체
    LiquidityCalculator liquidity_calc_;
    SlippageModel slippage_model_;

    // 동기화
    mutable std::shared_mutex mutex_;

    // 콜백
    LiquidityAlertCallback alert_callback_;

    // 통계
    mutable Stats stats_;
};

}  // namespace arbitrage
