// TASK_10: OrderBook Analyzer Test
// 오더북 분석기 기능 테스트

#include "arbitrage/common/types.hpp"
#include "arbitrage/strategy/liquidity_metrics.hpp"
#include "arbitrage/strategy/slippage_model.hpp"
#include "arbitrage/strategy/orderbook_analyzer.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstring>

using namespace arbitrage;

// 테스트 결과 카운터
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    std::cout << "\n=== Test: " << name << " ===" << std::endl; \
    bool test_passed = true;

#define EXPECT_TRUE(cond, msg) \
    if (!(cond)) { \
        std::cout << "  [FAIL] " << msg << std::endl; \
        test_passed = false; \
    } else { \
        std::cout << "  [PASS] " << msg << std::endl; \
    }

#define EXPECT_NEAR(a, b, eps, msg) \
    if (std::abs((a) - (b)) > (eps)) { \
        std::cout << "  [FAIL] " << msg << " (expected " << b << ", got " << a << ")" << std::endl; \
        test_passed = false; \
    } else { \
        std::cout << "  [PASS] " << msg << std::endl; \
    }

#define END_TEST() \
    if (test_passed) { \
        std::cout << ">>> PASSED <<<" << std::endl; \
        tests_passed++; \
    } else { \
        std::cout << ">>> FAILED <<<" << std::endl; \
        tests_failed++; \
    }

// 테스트용 오더북 생성
OrderBook create_test_orderbook(Exchange ex, double mid_price, double spread_pct = 0.1) {
    OrderBook ob;
    ob.exchange = ex;
    ob.set_symbol(is_krw_exchange(ex) ? "KRW-XRP" : "XRPUSDT");
    ob.clear();

    double half_spread = mid_price * spread_pct / 100.0 / 2.0;
    double best_bid = mid_price - half_spread;
    double best_ask = mid_price + half_spread;

    // 매수 호가 (내림차순)
    for (int i = 0; i < 10; ++i) {
        double price = best_bid - i * (best_bid * 0.001);  // 0.1%씩 낮아짐
        double qty = 100.0 + i * 50.0;  // 깊이로 갈수록 수량 증가
        ob.add_bid(price, qty);
    }

    // 매도 호가 (오름차순)
    for (int i = 0; i < 10; ++i) {
        double price = best_ask + i * (best_ask * 0.001);  // 0.1%씩 높아짐
        double qty = 100.0 + i * 50.0;
        ob.add_ask(price, qty);
    }

    return ob;
}

// Test 1: Liquidity Metrics 계산
void test_liquidity_metrics() {
    TEST("Liquidity Metrics Calculation");

    LiquidityCalculator calc;
    calc.set_depth_range_pct(1.0);  // 1% 범위

    OrderBook ob = create_test_orderbook(Exchange::Upbit, 3100.0, 0.1);  // 3100 KRW, 0.1% 스프레드

    LiquidityMetrics metrics = calc.calculate(ob);

    EXPECT_TRUE(metrics.is_valid(), "Metrics should be valid");
    EXPECT_TRUE(metrics.best_bid > 0.0, "Best bid should be positive");
    EXPECT_TRUE(metrics.best_ask > 0.0, "Best ask should be positive");
    EXPECT_TRUE(metrics.best_bid < metrics.best_ask, "Best bid < best ask");
    EXPECT_TRUE(metrics.spread > 0.0, "Spread should be positive");
    EXPECT_TRUE(metrics.spread_bps > 0.0, "Spread (bps) should be positive");
    EXPECT_TRUE(metrics.bid_depth_1pct > 0.0, "Bid depth should be positive");
    EXPECT_TRUE(metrics.ask_depth_1pct > 0.0, "Ask depth should be positive");
    EXPECT_TRUE(metrics.bid_levels > 0, "Bid levels should be counted");
    EXPECT_TRUE(metrics.ask_levels > 0, "Ask levels should be counted");

    std::cout << "  Best Bid: " << metrics.best_bid << std::endl;
    std::cout << "  Best Ask: " << metrics.best_ask << std::endl;
    std::cout << "  Spread: " << metrics.spread_bps << " bps" << std::endl;
    std::cout << "  Bid Depth (1%): " << metrics.bid_depth_1pct << " qty" << std::endl;
    std::cout << "  Ask Depth (1%): " << metrics.ask_depth_1pct << " qty" << std::endl;
    std::cout << "  Imbalance: " << metrics.imbalance << std::endl;

    END_TEST();
}

// Test 2: 슬리피지 예측 (매수)
void test_slippage_buy() {
    TEST("Slippage Estimation (Buy)");

    SlippageModel model;
    OrderBook ob = create_test_orderbook(Exchange::Binance, 2.15, 0.1);  // 2.15 USDT

    double quantity = 500.0;  // 500 XRP 매수
    SlippageEstimate est = model.estimate_taker_slippage(ob, OrderSide::Buy, quantity);

    EXPECT_TRUE(est.is_valid(), "Estimate should be valid");
    EXPECT_TRUE(est.expected_avg_price >= est.best_price, "Avg price >= best price for buy");
    EXPECT_TRUE(est.slippage_bps >= 0.0, "Slippage should be non-negative");
    EXPECT_TRUE(est.levels_consumed > 0, "Should consume at least 1 level");

    std::cout << "  Quantity: " << est.quantity << std::endl;
    std::cout << "  Best Ask: " << est.best_price << std::endl;
    std::cout << "  Expected Avg Price: " << est.expected_avg_price << std::endl;
    std::cout << "  Slippage: " << est.slippage_bps << " bps" << std::endl;
    std::cout << "  Levels Consumed: " << est.levels_consumed << std::endl;
    std::cout << "  Fill Ratio: " << est.fill_ratio * 100.0 << "%" << std::endl;
    std::cout << "  Fully Fillable: " << (est.fully_fillable ? "Yes" : "No") << std::endl;

    END_TEST();
}

// Test 3: 슬리피지 예측 (매도)
void test_slippage_sell() {
    TEST("Slippage Estimation (Sell)");

    SlippageModel model;
    OrderBook ob = create_test_orderbook(Exchange::Upbit, 3100.0, 0.1);

    double quantity = 500.0;  // 500 XRP 매도
    SlippageEstimate est = model.estimate_taker_slippage(ob, OrderSide::Sell, quantity);

    EXPECT_TRUE(est.is_valid(), "Estimate should be valid");
    EXPECT_TRUE(est.expected_avg_price <= est.best_price, "Avg price <= best price for sell");
    EXPECT_TRUE(est.slippage_bps >= 0.0, "Slippage should be non-negative");

    std::cout << "  Quantity: " << est.quantity << std::endl;
    std::cout << "  Best Bid: " << est.best_price << std::endl;
    std::cout << "  Expected Avg Price: " << est.expected_avg_price << std::endl;
    std::cout << "  Slippage: " << est.slippage_bps << " bps" << std::endl;
    std::cout << "  Levels Consumed: " << est.levels_consumed << std::endl;

    END_TEST();
}

// Test 4: 대량 주문 슬리피지 (부분 체결)
void test_large_order_slippage() {
    TEST("Large Order Slippage (Partial Fill)");

    SlippageModel model;
    OrderBook ob = create_test_orderbook(Exchange::Binance, 2.15, 0.1);

    // 오더북의 모든 물량보다 큰 수량 주문
    double large_qty = 10000.0;  // 10000 XRP
    SlippageEstimate est = model.estimate_taker_slippage(ob, OrderSide::Buy, large_qty);

    EXPECT_TRUE(est.fill_ratio < 1.0, "Fill ratio should be < 1 for large order");
    EXPECT_TRUE(!est.fully_fillable, "Should not be fully fillable");
    EXPECT_TRUE(est.fillable_qty < large_qty, "Fillable qty should be less than ordered");
    EXPECT_TRUE(est.slippage_bps > 0.0, "Should have significant slippage");

    std::cout << "  Ordered: " << large_qty << std::endl;
    std::cout << "  Fillable: " << est.fillable_qty << std::endl;
    std::cout << "  Fill Ratio: " << est.fill_ratio * 100.0 << "%" << std::endl;
    std::cout << "  Slippage: " << est.slippage_bps << " bps" << std::endl;
    std::cout << "  Worst Price: " << est.worst_price << std::endl;

    END_TEST();
}

// Test 5: Maker 가격 계산
void test_maker_price() {
    TEST("Maker Price Calculation");

    SlippageModel model;
    OrderBook ob = create_test_orderbook(Exchange::Binance, 2.15, 0.1);

    // 80% 체결 확률 목표
    auto estimate = model.calculate_optimal_maker_price(ob, OrderSide::Buy, 0.8, 30.0);

    EXPECT_TRUE(estimate.is_valid(), "Estimate should be valid");
    EXPECT_TRUE(estimate.recommended_price <= estimate.best_price, "Maker buy price <= best bid");
    EXPECT_NEAR(estimate.estimated_fill_prob, 0.8, 0.01, "Fill probability should be ~80%");

    std::cout << "  Best Bid: " << estimate.best_price << std::endl;
    std::cout << "  Recommended Price: " << estimate.recommended_price << std::endl;
    std::cout << "  Distance from Best: " << estimate.distance_from_best_bps << " bps" << std::endl;
    std::cout << "  Estimated Fill Prob: " << estimate.estimated_fill_prob * 100.0 << "%" << std::endl;
    std::cout << "  Estimated Wait: " << estimate.estimated_wait_sec << " sec" << std::endl;

    END_TEST();
}

// Test 6: OrderBookAnalyzer 기본 기능
void test_orderbook_analyzer_basic() {
    TEST("OrderBookAnalyzer Basic");

    OrderBookAnalyzer analyzer;

    // 오더북 업데이트
    OrderBook upbit_ob = create_test_orderbook(Exchange::Upbit, 3100.0, 0.1);
    OrderBook binance_ob = create_test_orderbook(Exchange::Binance, 2.15, 0.1);

    analyzer.update(Exchange::Upbit, upbit_ob);
    analyzer.update(Exchange::Binance, binance_ob);

    // 유동성 조회
    auto upbit_liq = analyzer.get_liquidity(Exchange::Upbit);
    auto binance_liq = analyzer.get_liquidity(Exchange::Binance);

    EXPECT_TRUE(upbit_liq.is_valid(), "Upbit liquidity should be valid");
    EXPECT_TRUE(binance_liq.is_valid(), "Binance liquidity should be valid");

    // 슬리피지 예측
    auto slip = analyzer.estimate_slippage(Exchange::Upbit, OrderSide::Sell, 500.0);
    EXPECT_TRUE(slip.is_valid(), "Slippage estimate should be valid");

    std::cout << "  Upbit Spread: " << upbit_liq.spread_bps << " bps" << std::endl;
    std::cout << "  Binance Spread: " << binance_liq.spread_bps << " bps" << std::endl;
    std::cout << "  Upbit Sell Slippage (500 XRP): " << slip.slippage_bps << " bps" << std::endl;

    // 통계 확인
    const auto& stats = analyzer.stats();
    EXPECT_TRUE(stats.updates.load() >= 2, "Should have at least 2 updates");
    EXPECT_TRUE(stats.queries.load() >= 3, "Should have at least 3 queries");

    END_TEST();
}

// Test 7: Maker+Taker 주문 계획
void test_dual_order_plan() {
    TEST("Dual Order Plan (Maker+Taker)");

    OrderBookAnalyzer analyzer;

    // 김프 3% 상황 시뮬레이션
    // 해외: 2.15 USDT → 3010 KRW (1400 환율 적용)
    // 국내: 3100 KRW → 3% 김프
    OrderBook upbit_ob = create_test_orderbook(Exchange::Upbit, 3100.0, 0.1);
    OrderBook binance_ob = create_test_orderbook(Exchange::Binance, 2.15, 0.1);

    analyzer.update(Exchange::Upbit, upbit_ob);
    analyzer.update(Exchange::Binance, binance_ob);

    // 1000 XRP 거래 계획
    double quantity = 1000.0;
    double fx_rate = 1400.0;

    DualOrderPlan plan = analyzer.plan_maker_taker_order(
        Exchange::Binance,  // 해외 매수 (Maker)
        Exchange::Upbit,    // 국내 매도 (Taker)
        quantity,
        fx_rate
    );

    EXPECT_TRUE(plan.is_valid(), "Plan should be valid");
    EXPECT_TRUE(plan.maker_exchange == Exchange::Binance, "Maker should be Binance");
    EXPECT_TRUE(plan.taker_exchange == Exchange::Upbit, "Taker should be Upbit");
    EXPECT_TRUE(plan.maker_price > 0.0, "Maker price should be positive");
    EXPECT_TRUE(plan.taker_price > 0.0, "Taker price should be positive");
    EXPECT_TRUE(plan.total_fee_value >= 0.0, "Total fee should be non-negative");

    std::cout << "  Maker (Binance): " << plan.maker_price << " USDT" << std::endl;
    std::cout << "  Taker (Upbit): " << plan.taker_price << " KRW" << std::endl;
    std::cout << "  Maker Fee Rate: " << plan.maker_fee_rate * 100.0 << "%" << std::endl;
    std::cout << "  Taker Fee Rate: " << plan.taker_fee_rate * 100.0 << "%" << std::endl;
    std::cout << "  Taker Slippage: " << plan.taker_slippage_bps << " bps" << std::endl;
    std::cout << "  Gross Premium: " << plan.gross_premium_pct << "%" << std::endl;
    std::cout << "  Net Premium: " << plan.net_premium_pct << "%" << std::endl;
    std::cout << "  Expected Profit: " << plan.expected_profit_value << " KRW" << std::endl;
    std::cout << "  Profitable: " << (plan.is_profitable() ? "Yes" : "No") << std::endl;

    END_TEST();
}

// Test 8: 손익분기 프리미엄 계산
void test_breakeven_premium() {
    TEST("Breakeven Premium Calculation");

    OrderBookAnalyzer analyzer;

    double breakeven = analyzer.calculate_breakeven_premium(
        Exchange::Binance,  // 매수
        Exchange::Upbit     // 매도
    );

    EXPECT_TRUE(breakeven > 0.0, "Breakeven premium should be positive");
    EXPECT_TRUE(breakeven < 1.0, "Breakeven premium should be reasonable (< 1%)");

    std::cout << "  Breakeven Premium (Binance->Upbit): " << breakeven << "%" << std::endl;

    // MEXC는 Maker 무료
    double breakeven_mexc = analyzer.calculate_breakeven_premium(
        Exchange::MEXC,     // 매수 (Maker 무료)
        Exchange::Upbit     // 매도
    );

    EXPECT_TRUE(breakeven_mexc < breakeven, "MEXC breakeven should be lower (free maker)");
    std::cout << "  Breakeven Premium (MEXC->Upbit): " << breakeven_mexc << "%" << std::endl;

    END_TEST();
}

// Test 9: 유동성 경고 콜백
void test_liquidity_alert() {
    TEST("Liquidity Alert Callback");

    OrderBookAnalyzer analyzer;

    // 낮은 유동성 임계값 설정
    OrderBookAnalyzerConfig config;
    config.min_depth_value = 1000000000.0;  // 10억원 (일부러 높게)
    config.max_spread_bps = 1.0;            // 0.01% (일부러 낮게)
    analyzer.set_config(config);

    int alert_count = 0;
    Exchange last_alert_exchange;
    LiquidityAlert last_alert_type;

    analyzer.set_alert_callback([&](Exchange ex, LiquidityAlert alert, const char* msg) {
        alert_count++;
        last_alert_exchange = ex;
        last_alert_type = alert;
        std::cout << "  Alert: " << exchange_name(ex) << " - "
                  << liquidity_alert_name(alert) << " - " << msg << std::endl;
    });

    // 일반 오더북 업데이트 (경고 발생해야 함)
    OrderBook ob = create_test_orderbook(Exchange::Upbit, 3100.0, 0.1);
    analyzer.update(Exchange::Upbit, ob);

    EXPECT_TRUE(alert_count > 0, "Should trigger at least one alert");

    std::cout << "  Total alerts triggered: " << alert_count << std::endl;

    END_TEST();
}

// Test 10: VWAP 계산
void test_vwap_calculation() {
    TEST("VWAP Calculation");

    PriceLevel levels[5] = {
        {100.0, 10.0},   // 1000
        {101.0, 20.0},   // 2020
        {102.0, 30.0},   // 3060
        {103.0, 40.0},   // 4120
        {104.0, 50.0}    // 5200
    };

    // 50 수량의 VWAP: (100*10 + 101*20 + 102*20) / 50 = 5060 / 50 = 101.2
    double vwap50 = vwap::calculate_vwap(levels, 5, 50.0);
    EXPECT_NEAR(vwap50, 101.2, 0.01, "VWAP for 50 qty");

    // 10 수량의 VWAP: 첫 번째 레벨만 → 100.0
    double vwap10 = vwap::calculate_vwap(levels, 5, 10.0);
    EXPECT_NEAR(vwap10, 100.0, 0.01, "VWAP for 10 qty");

    // 전체 수량 VWAP
    // Total qty = 150, Total value = 15400
    // VWAP = 15400 / 150 = 102.67
    double vwap_all = vwap::calculate_vwap(levels, 5, 200.0);
    EXPECT_NEAR(vwap_all, 102.67, 0.01, "VWAP for all qty");

    std::cout << "  VWAP (50 qty): " << vwap50 << std::endl;
    std::cout << "  VWAP (10 qty): " << vwap10 << std::endl;
    std::cout << "  VWAP (all): " << vwap_all << std::endl;

    END_TEST();
}

int main() {
    std::cout << "======================================" << std::endl;
    std::cout << "  TASK_10: OrderBook Analyzer Test" << std::endl;
    std::cout << "======================================" << std::endl;

    test_liquidity_metrics();
    test_slippage_buy();
    test_slippage_sell();
    test_large_order_slippage();
    test_maker_price();
    test_orderbook_analyzer_basic();
    test_dual_order_plan();
    test_breakeven_premium();
    test_liquidity_alert();
    test_vwap_calculation();

    std::cout << "\n======================================" << std::endl;
    std::cout << "  Test Results" << std::endl;
    std::cout << "======================================" << std::endl;
    std::cout << "Passed: " << tests_passed << std::endl;
    std::cout << "Failed: " << tests_failed << std::endl;
    std::cout << "Total:  " << (tests_passed + tests_failed) << std::endl;

    if (tests_failed == 0) {
        std::cout << "\n*** ALL TESTS PASSED ***" << std::endl;
        return 0;
    } else {
        std::cout << "\n*** SOME TESTS FAILED ***" << std::endl;
        return 1;
    }
}
