/**
 * Fee Calculator Test (TASK_11)
 *
 * 거래소별 수수료 계산 테스트
 * - Maker/Taker 수수료
 * - VIP 등급 할인
 * - 토큰 할인 (BNB, MX)
 * - 출금 수수료
 * - 아비트라지 총 비용 계산
 */

#include "arbitrage/common/fee_calculator.hpp"
#include "arbitrage/common/fee_constants.hpp"

#include <iostream>
#include <iomanip>
#include <cmath>
#include <string>

using namespace arbitrage;

// 테스트 결과 카운터
static int tests_passed = 0;
static int tests_failed = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::cout << "[FAIL] " << msg << "\n";
        ++tests_failed;
    } else {
        std::cout << "[PASS] " << msg << "\n";
        ++tests_passed;
    }
}

void check_near(double a, double b, double eps, const char* msg) {
    check(std::abs(a - b) < eps, msg);
}

// =============================================================================
// Test: Default Fee Rates
// =============================================================================
void test_default_fee_rates() {
    std::cout << "\n=== Test: default_fee_rates ===\n";

    FeeCalculator calc;

    // Upbit: 0.05%
    double upbit_maker = calc.get_fee_rate(Exchange::Upbit, OrderRole::Maker);
    double upbit_taker = calc.get_fee_rate(Exchange::Upbit, OrderRole::Taker);
    check_near(upbit_maker, 0.0005, 0.0001, "Upbit Maker fee = 0.05%");
    check_near(upbit_taker, 0.0005, 0.0001, "Upbit Taker fee = 0.05%");

    // Bithumb: 0.04%
    double bithumb_maker = calc.get_fee_rate(Exchange::Bithumb, OrderRole::Maker);
    check_near(bithumb_maker, 0.0004, 0.0001, "Bithumb Maker fee = 0.04%");

    // Binance: 0.10%
    double binance_taker = calc.get_fee_rate(Exchange::Binance, OrderRole::Taker);
    check_near(binance_taker, 0.001, 0.0001, "Binance Taker fee = 0.10%");

    // MEXC: Maker 무료
    double mexc_maker = calc.get_fee_rate(Exchange::MEXC, OrderRole::Maker);
    check_near(mexc_maker, 0.0, 0.0001, "MEXC Maker fee = 0%");

    // MEXC: Taker 0.02%
    double mexc_taker = calc.get_fee_rate(Exchange::MEXC, OrderRole::Taker);
    check_near(mexc_taker, 0.0002, 0.0001, "MEXC Taker fee = 0.02%");
}

// =============================================================================
// Test: Withdraw Fees
// =============================================================================
void test_withdraw_fees() {
    std::cout << "\n=== Test: withdraw_fees ===\n";

    FeeCalculator calc;

    // Upbit XRP: 1.0 XRP
    double upbit_xrp = calc.get_withdraw_fee(Exchange::Upbit, "XRP");
    check_near(upbit_xrp, 1.0, 0.01, "Upbit XRP withdraw = 1.0");

    // Binance XRP: 0.25 XRP
    double binance_xrp = calc.get_withdraw_fee(Exchange::Binance, "XRP");
    check_near(binance_xrp, 0.25, 0.01, "Binance XRP withdraw = 0.25");

    // MEXC XRP: 0.25 XRP
    double mexc_xrp = calc.get_withdraw_fee(Exchange::MEXC, "XRP");
    check_near(mexc_xrp, 0.25, 0.01, "MEXC XRP withdraw = 0.25");
}

// =============================================================================
// Test: Minimum Withdraw
// =============================================================================
void test_min_withdraw() {
    std::cout << "\n=== Test: min_withdraw ===\n";

    FeeCalculator calc;

    double upbit_min = calc.get_min_withdraw(Exchange::Upbit, "XRP");
    check(upbit_min > 0, "Upbit min withdraw > 0");

    double binance_min = calc.get_min_withdraw(Exchange::Binance, "XRP");
    check(binance_min >= 25.0, "Binance min withdraw >= 25");
}

// =============================================================================
// Test: Trade Cost Calculation
// =============================================================================
void test_trade_cost() {
    std::cout << "\n=== Test: trade_cost ===\n";

    FeeCalculator calc;

    // 1000 XRP @ 3000 KRW
    TradeCost cost = calc.calculate_trade_cost(
        Exchange::Upbit, OrderRole::Taker,
        1000.0,  // quantity
        3000.0,  // price
        1.0      // fx_rate (KRW)
    );

    std::cout << "  Notional: " << cost.notional_krw << " KRW\n";
    std::cout << "  Fee Rate: " << cost.fee_rate_pct << "%\n";
    std::cout << "  Fee: " << cost.fee_krw << " KRW\n";

    check_near(cost.notional_krw, 3000000.0, 1.0, "Notional = 3,000,000 KRW");
    check_near(cost.fee_rate_pct, 0.05, 0.01, "Fee rate = 0.05%");
    check_near(cost.fee_krw, 1500.0, 1.0, "Fee = 1,500 KRW");
}

// =============================================================================
// Test: Trade Cost with FX Rate
// =============================================================================
void test_trade_cost_fx() {
    std::cout << "\n=== Test: trade_cost_fx ===\n";

    FeeCalculator calc;

    // 1000 XRP @ 2.1 USDT, fx_rate = 1400
    TradeCost cost = calc.calculate_trade_cost(
        Exchange::Binance, OrderRole::Maker,
        1000.0,  // quantity
        2.1,     // price (USDT)
        1400.0   // fx_rate
    );

    std::cout << "  Notional: " << cost.notional << " USDT\n";
    std::cout << "  Notional KRW: " << cost.notional_krw << " KRW\n";
    std::cout << "  Fee Rate: " << cost.fee_rate_pct << "%\n";
    std::cout << "  Fee KRW: " << cost.fee_krw << " KRW\n";

    check_near(cost.notional, 2100.0, 1.0, "Notional = 2,100 USDT");
    check_near(cost.notional_krw, 2940000.0, 1000.0, "Notional KRW = 2,940,000");
    check_near(cost.fee_rate_pct, 0.10, 0.01, "Binance Maker fee = 0.10%");
}

// =============================================================================
// Test: Transfer Cost
// =============================================================================
void test_transfer_cost() {
    std::cout << "\n=== Test: transfer_cost ===\n";

    FeeCalculator calc;

    // Binance -> Upbit, 1000 XRP @ 3000 KRW
    TransferCost cost = calc.calculate_transfer_cost(
        "XRP",
        1000.0,           // amount
        Exchange::Binance,
        Exchange::Upbit,
        3000.0            // price KRW
    );

    std::cout << "  Withdraw Fee: " << cost.withdraw_fee << " XRP\n";
    std::cout << "  Withdraw Fee KRW: " << cost.withdraw_fee_krw << " KRW\n";
    std::cout << "  Net Amount: " << cost.net_amount << " XRP\n";

    check_near(cost.withdraw_fee, 0.25, 0.01, "Binance XRP withdraw = 0.25");
    check_near(cost.withdraw_fee_krw, 750.0, 10.0, "Withdraw fee = 750 KRW");
    check_near(cost.net_amount, 999.75, 0.01, "Net amount = 999.75 XRP");
}

// =============================================================================
// Test: Arbitrage Cost - Binance to Upbit
// =============================================================================
void test_arbitrage_cost_binance_upbit() {
    std::cout << "\n=== Test: arbitrage_cost_binance_upbit ===\n";

    FeeCalculator calc;

    // Binance (Maker) -> Upbit (Taker)
    // 1000 XRP @ 2.1 USDT / 3000 KRW
    // 환율 1400
    ArbitrageCost cost = calc.calculate_arbitrage_cost(
        Exchange::Binance, Exchange::Upbit,
        1000.0,    // quantity
        2.1,       // buy price (USDT)
        3000.0,    // sell price (KRW)
        1400.0,    // fx_rate
        OrderRole::Maker,
        OrderRole::Taker
    );

    std::cout << "\n  [Arbitrage: Binance -> Upbit]\n";
    std::cout << "  Buy price (KRW): " << cost.buy_price_krw << "\n";
    std::cout << "  Sell price (KRW): " << cost.sell_price_krw << "\n";
    std::cout << "  Gross premium: " << cost.gross_premium_pct << "%\n";
    std::cout << "  Total fee: " << cost.total_fee_krw << " KRW ("
              << cost.total_fee_pct << "%)\n";
    std::cout << "  Net profit: " << cost.net_profit_krw << " KRW ("
              << cost.net_profit_pct << "%)\n";
    std::cout << "  Profitable: " << (cost.is_profitable() ? "YES" : "NO") << "\n";

    check(cost.buy_price_krw > 0, "Buy price calculated");
    check(cost.sell_price_krw > 0, "Sell price calculated");
    check(cost.gross_premium_pct > 0, "Gross premium positive (김프)");
    check(cost.total_fee_pct > 0, "Total fee calculated");
}

// =============================================================================
// Test: Arbitrage Cost - MEXC to Upbit (Optimal)
// =============================================================================
void test_arbitrage_cost_mexc_upbit() {
    std::cout << "\n=== Test: arbitrage_cost_mexc_upbit ===\n";

    FeeCalculator calc;

    // MEXC (Maker 무료!) -> Upbit (Taker)
    ArbitrageCost cost = calc.calculate_arbitrage_cost(
        Exchange::MEXC, Exchange::Upbit,
        1000.0,
        2.1,
        3000.0,
        1400.0,
        OrderRole::Maker,
        OrderRole::Taker
    );

    std::cout << "\n  [Arbitrage: MEXC -> Upbit (Optimal)]\n";
    std::cout << "  Buy fee: " << cost.buy_cost.fee_rate_pct << "% (MEXC Maker FREE)\n";
    std::cout << "  Sell fee: " << cost.sell_cost.fee_rate_pct << "%\n";
    std::cout << "  Total fee: " << cost.total_fee_pct << "%\n";
    std::cout << "  Net profit: " << cost.net_profit_pct << "%\n";

    // MEXC Maker는 무료
    check_near(cost.buy_cost.fee_rate_pct, 0.0, 0.01, "MEXC Maker fee = 0%");

    // MEXC 경로가 Binance보다 수수료 저렴
    check(cost.total_fee_pct < 0.2, "MEXC route: total fee < 0.2%");
}

// =============================================================================
// Test: Breakeven Premium
// =============================================================================
void test_breakeven_premium() {
    std::cout << "\n=== Test: breakeven_premium ===\n";

    FeeCalculator calc;

    // Binance -> Upbit
    double be_binance = calc.calculate_breakeven_premium(
        Exchange::Binance, Exchange::Upbit,
        OrderRole::Maker, OrderRole::Taker
    );

    // MEXC -> Upbit
    double be_mexc = calc.calculate_breakeven_premium(
        Exchange::MEXC, Exchange::Upbit,
        OrderRole::Maker, OrderRole::Taker
    );

    std::cout << "  Binance -> Upbit breakeven: " << be_binance << "%\n";
    std::cout << "  MEXC -> Upbit breakeven: " << be_mexc << "%\n";

    check(be_binance > 0, "Binance breakeven > 0");
    check(be_mexc < be_binance, "MEXC breakeven < Binance (MEXC is cheaper)");
    check(be_mexc < 0.15, "MEXC breakeven < 0.15%");
}

// =============================================================================
// Test: VIP Level
// =============================================================================
void test_vip_level() {
    std::cout << "\n=== Test: vip_level ===\n";

    FeeCalculator calc;

    // Binance VIP 0 (default)
    double fee_vip0 = calc.get_fee_rate_pct(Exchange::Binance, OrderRole::Maker);
    std::cout << "  Binance VIP 0 Maker: " << fee_vip0 << "%\n";

    // Set VIP 1
    calc.set_vip_level(Exchange::Binance, 1);
    double fee_vip1 = calc.get_fee_rate_pct(Exchange::Binance, OrderRole::Maker);
    std::cout << "  Binance VIP 1 Maker: " << fee_vip1 << "%\n";

    // VIP 1 should be cheaper
    check(fee_vip1 < fee_vip0, "VIP 1 fee < VIP 0 fee");

    // Set VIP 3
    calc.set_vip_level(Exchange::Binance, 3);
    double fee_vip3 = calc.get_fee_rate_pct(Exchange::Binance, OrderRole::Maker);
    std::cout << "  Binance VIP 3 Maker: " << fee_vip3 << "%\n";

    check(fee_vip3 < fee_vip1, "VIP 3 fee < VIP 1 fee");
}

// =============================================================================
// Test: Token Discount
// =============================================================================
void test_token_discount() {
    std::cout << "\n=== Test: token_discount ===\n";

    FeeCalculator calc;

    // Binance without BNB discount
    double fee_no_bnb = calc.get_fee_rate(Exchange::Binance, OrderRole::Taker);
    std::cout << "  Binance Taker (no BNB): " << fee_no_bnb * 100.0 << "%\n";

    // Enable BNB discount (25%)
    calc.set_token_discount(Exchange::Binance, true);
    double fee_with_bnb = calc.get_fee_rate(Exchange::Binance, OrderRole::Taker);
    std::cout << "  Binance Taker (with BNB): " << fee_with_bnb * 100.0 << "%\n";

    // 25% discount: 0.10% -> 0.075%
    check(fee_with_bnb < fee_no_bnb, "BNB discount applied");
    check_near(fee_with_bnb, 0.00075, 0.0001, "Binance with BNB = 0.075%");
}

// =============================================================================
// Test: Update Withdraw Fee
// =============================================================================
void test_update_withdraw_fee() {
    std::cout << "\n=== Test: update_withdraw_fee ===\n";

    FeeCalculator calc;

    double original = calc.get_withdraw_fee(Exchange::Binance, "XRP");
    std::cout << "  Original: " << original << " XRP\n";

    // 네트워크 혼잡으로 수수료 증가
    calc.update_withdraw_fee(Exchange::Binance, "XRP", 0.5);
    double updated = calc.get_withdraw_fee(Exchange::Binance, "XRP");
    std::cout << "  Updated: " << updated << " XRP\n";

    check_near(updated, 0.5, 0.01, "Withdraw fee updated to 0.5");
}

// =============================================================================
// Test: Constexpr Fee Constants
// =============================================================================
void test_constexpr_fees() {
    std::cout << "\n=== Test: constexpr_fees ===\n";

    // 컴파일 타임 계산 테스트
    constexpr double upbit_maker = fee::maker_fee(Exchange::Upbit);
    constexpr double mexc_maker = fee::maker_fee(Exchange::MEXC);
    constexpr double binance_taker = fee::taker_fee(Exchange::Binance);

    std::cout << "  Constexpr Upbit Maker: " << upbit_maker * 100.0 << "%\n";
    std::cout << "  Constexpr MEXC Maker: " << mexc_maker * 100.0 << "%\n";
    std::cout << "  Constexpr Binance Taker: " << binance_taker * 100.0 << "%\n";

    check_near(upbit_maker, 0.0005, 0.0001, "Constexpr Upbit = 0.05%");
    check_near(mexc_maker, 0.0, 0.0001, "Constexpr MEXC Maker = 0%");

    // 손익분기 프리미엄 (constexpr)
    constexpr double be = fee::breakeven_premium(Exchange::Binance, Exchange::Upbit);
    std::cout << "  Constexpr breakeven: " << be * 100.0 << "%\n";

    check(be > 0, "Constexpr breakeven > 0");
}

// =============================================================================
// Test: Print Summary
// =============================================================================
void test_print_summary() {
    std::cout << "\n=== Test: print_summary ===\n";

    FeeCalculator calc;
    calc.print_summary();
    check(true, "Print summary completed");
}

// =============================================================================
// Test: Validate
// =============================================================================
void test_validate() {
    std::cout << "\n=== Test: validate ===\n";

    FeeCalculator calc;
    bool valid = calc.validate();
    check(valid, "Default config is valid");
}

// =============================================================================
// Main
// =============================================================================
int main() {
    std::cout << "================================================\n";
    std::cout << " Fee Calculator Test (TASK_11)\n";
    std::cout << "================================================\n";

    test_default_fee_rates();
    test_withdraw_fees();
    test_min_withdraw();
    test_trade_cost();
    test_trade_cost_fx();
    test_transfer_cost();
    test_arbitrage_cost_binance_upbit();
    test_arbitrage_cost_mexc_upbit();
    test_breakeven_premium();
    test_vip_level();
    test_token_discount();
    test_update_withdraw_fee();
    test_constexpr_fees();
    test_print_summary();
    test_validate();

    std::cout << "\n================================================\n";
    std::cout << " Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "================================================\n";

    return tests_failed > 0 ? 1 : 0;
}
