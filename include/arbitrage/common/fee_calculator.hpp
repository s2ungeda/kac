#pragma once

/**
 * Fee Calculator (TASK_11)
 *
 * 거래소별 정확한 수수료 계산
 * - Maker/Taker 수수료 구분
 * - VIP 등급별 할인
 * - 거래소 토큰 할인 (BNB, MX)
 * - 출금 수수료 (코인별)
 * - 아비트라지 총 비용 계산
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/fee_constants.hpp"
#include "arbitrage/common/spin_wait.hpp"

#include <map>
#include <string>
#include <vector>
#include <functional>

namespace arbitrage {

// =============================================================================
// 주문 역할 (Maker/Taker)
// =============================================================================
enum class OrderRole : uint8_t {
    Maker,      // 지정가 (호가 제공)
    Taker       // 시장가 (호가 소비)
};

constexpr const char* order_role_name(OrderRole role) {
    return role == OrderRole::Maker ? "Maker" : "Taker";
}

// =============================================================================
// 거래소별 수수료 설정
// =============================================================================
struct ExchangeFeeConfig {
    Exchange exchange{Exchange::Upbit};

    // 거래 수수료 (%, 예: 0.05 = 0.05%)
    double maker_fee_pct{0.05};
    double taker_fee_pct{0.05};

    // 할인 옵션
    double token_discount_pct{0.0};     // 거래소 토큰 사용 시 할인율 (%)
    bool use_token_for_fee{false};       // 토큰으로 수수료 지불 여부

    // VIP 등급 (0 = 일반)
    int vip_level{0};

    // 출금 수수료 (코인별)
    std::map<std::string, double> withdraw_fees;  // {"XRP": 0.25, "BTC": 0.0005}

    // 최소 출금 수량 (코인별)
    std::map<std::string, double> min_withdraw;   // {"XRP": 25, "BTC": 0.001}

    // 수수료율 반환 (비율, 예: 0.0005 = 0.05%)
    double get_fee_rate(OrderRole role) const {
        double base = (role == OrderRole::Maker) ? maker_fee_pct : taker_fee_pct;
        double rate = base / 100.0;

        // 토큰 할인 적용
        if (use_token_for_fee && token_discount_pct > 0) {
            rate *= (1.0 - token_discount_pct / 100.0);
        }
        return rate;
    }
};

// =============================================================================
// VIP 등급별 수수료 테이블
// =============================================================================
struct VipFeeLevel {
    int level{0};
    double maker_fee_pct{0.0};
    double taker_fee_pct{0.0};
};

// =============================================================================
// 거래 비용 상세
// =============================================================================
struct TradeCost {
    Exchange exchange{Exchange::Upbit};
    OrderRole role{OrderRole::Taker};

    double quantity{0.0};           // 거래 수량
    double price{0.0};              // 거래 가격
    double notional{0.0};           // 거래 금액 (원 통화)
    double notional_krw{0.0};       // 거래 금액 (KRW 환산)

    double fee_rate_pct{0.0};       // 적용 수수료율 (%)
    double fee_rate{0.0};           // 적용 수수료율 (비율)
    double fee{0.0};                // 수수료 (원 통화)
    double fee_krw{0.0};            // 수수료 (KRW 환산)
    double fee_coin{0.0};           // 수수료 (코인, 토큰 지불 시)

    double net_quantity{0.0};       // 수수료 차감 후 수량
    double effective_price{0.0};    // 실효 가격 (수수료 반영)

    bool use_token_fee{false};      // 토큰으로 수수료 지불 여부
};

// =============================================================================
// 송금 비용 상세
// =============================================================================
struct TransferCost {
    std::string coin{"XRP"};
    double amount{0.0};
    Exchange from{Exchange::Binance};
    Exchange to{Exchange::Upbit};

    double withdraw_fee{0.0};           // 출금 수수료 (코인)
    double withdraw_fee_krw{0.0};       // 출금 수수료 (KRW 환산)
    double network_fee{0.0};            // 네트워크 수수료 (별도 있을 경우)

    double net_amount{0.0};             // 실수령 수량
    double total_cost_krw{0.0};         // 총 비용 (KRW)
};

// =============================================================================
// 아비트라지 총 비용
// =============================================================================
struct ArbitrageCost {
    TradeCost buy_cost;             // 매수 비용
    TradeCost sell_cost;            // 매도 비용
    TransferCost transfer_cost;     // 송금 비용

    double quantity{0.0};           // 거래 수량
    double buy_price_krw{0.0};      // 매수 가격 (KRW 환산)
    double sell_price_krw{0.0};     // 매도 가격 (KRW)

    double gross_premium{0.0};      // 세전 프리미엄 금액 (KRW)
    double gross_premium_pct{0.0};  // 세전 프리미엄 (%)

    double total_fee_krw{0.0};      // 총 수수료 (KRW)
    double total_fee_pct{0.0};      // 총 수수료율 (%)

    double net_profit_krw{0.0};     // 순수익 (KRW)
    double net_profit_pct{0.0};     // 순수익률 (%)

    bool is_profitable() const { return net_profit_krw > 0; }

    // 손익분기 프리미엄 (이 이상이어야 수익)
    double breakeven_premium_pct() const { return total_fee_pct; }
};

// =============================================================================
// Fee Calculator
// =============================================================================
class FeeCalculator {
public:
    FeeCalculator();
    ~FeeCalculator() = default;

    // 복사/이동 금지
    FeeCalculator(const FeeCalculator&) = delete;
    FeeCalculator& operator=(const FeeCalculator&) = delete;

    // =========================================================================
    // 설정 로드
    // =========================================================================

    /**
     * YAML 설정 파일 로드
     * @param config_path fees.yaml 경로
     * @return 로드 성공 여부
     */
    bool load_config(const std::string& config_path);

    /**
     * 거래소별 설정 적용
     */
    void set_exchange_config(const ExchangeFeeConfig& config);

    /**
     * 거래소 설정 조회
     */
    const ExchangeFeeConfig& get_exchange_config(Exchange ex) const;

    // =========================================================================
    // 수수료율 조회
    // =========================================================================

    /**
     * 거래 수수료율 조회 (비율)
     * @param ex 거래소
     * @param role Maker/Taker
     * @return 수수료율 (예: 0.0005 = 0.05%)
     */
    double get_fee_rate(Exchange ex, OrderRole role) const;

    /**
     * 거래 수수료율 조회 (%)
     */
    double get_fee_rate_pct(Exchange ex, OrderRole role) const;

    /**
     * 출금 수수료 조회
     * @param ex 거래소
     * @param coin 코인 심볼 (예: "XRP")
     * @return 출금 수수료 (코인 수량)
     */
    double get_withdraw_fee(Exchange ex, const std::string& coin) const;

    /**
     * 최소 출금 수량 조회
     */
    double get_min_withdraw(Exchange ex, const std::string& coin) const;

    // =========================================================================
    // 비용 계산
    // =========================================================================

    /**
     * 거래 수수료 계산
     * @param ex 거래소
     * @param role Maker/Taker
     * @param quantity 거래 수량
     * @param price 거래 가격
     * @param fx_rate 환율 (USDT→KRW, 국내 거래소면 1.0)
     * @return 거래 비용 상세
     */
    TradeCost calculate_trade_cost(
        Exchange ex,
        OrderRole role,
        double quantity,
        double price,
        double fx_rate = 1.0
    ) const;

    /**
     * 송금 수수료 계산
     * @param coin 코인 심볼
     * @param amount 송금 수량
     * @param from 출발 거래소
     * @param to 도착 거래소
     * @param coin_price_krw 코인 가격 (KRW)
     * @return 송금 비용 상세
     */
    TransferCost calculate_transfer_cost(
        const std::string& coin,
        double amount,
        Exchange from,
        Exchange to,
        double coin_price_krw
    ) const;

    /**
     * 아비트라지 총 비용 계산
     * @param buy_ex 매수 거래소 (해외)
     * @param sell_ex 매도 거래소 (국내)
     * @param quantity 거래 수량
     * @param buy_price 매수 가격 (USDT)
     * @param sell_price 매도 가격 (KRW)
     * @param fx_rate 환율 (USDT→KRW)
     * @param buy_role 매수 주문 역할 (보통 Maker)
     * @param sell_role 매도 주문 역할 (보통 Taker)
     * @return 아비트라지 비용 상세
     */
    ArbitrageCost calculate_arbitrage_cost(
        Exchange buy_ex,
        Exchange sell_ex,
        double quantity,
        double buy_price,
        double sell_price,
        double fx_rate,
        OrderRole buy_role = OrderRole::Maker,
        OrderRole sell_role = OrderRole::Taker
    ) const;

    /**
     * 손익분기 프리미엄 계산
     * 최소 몇 % 김프여야 수익인가?
     * @param buy_ex 매수 거래소
     * @param sell_ex 매도 거래소
     * @param buy_role 매수 역할
     * @param sell_role 매도 역할
     * @return 손익분기 프리미엄 (%)
     */
    double calculate_breakeven_premium(
        Exchange buy_ex,
        Exchange sell_ex,
        OrderRole buy_role = OrderRole::Maker,
        OrderRole sell_role = OrderRole::Taker
    ) const;

    // =========================================================================
    // 설정 변경
    // =========================================================================

    /**
     * VIP 등급 설정
     */
    void set_vip_level(Exchange ex, int level);

    /**
     * 토큰 할인 설정
     */
    void set_token_discount(Exchange ex, bool enabled);

    /**
     * 출금 수수료 업데이트 (실시간)
     */
    void update_withdraw_fee(Exchange ex, const std::string& coin, double fee);

    // =========================================================================
    // 유틸리티
    // =========================================================================

    /**
     * 모든 거래소의 설정 요약 출력
     */
    void print_summary() const;

    /**
     * 설정 유효성 검사
     */
    bool validate() const;

private:
    // 기본 설정 초기화
    void init_default_configs();

    // VIP 등급 수수료 적용
    void apply_vip_fees(ExchangeFeeConfig& config) const;

    // 설정 저장소
    std::map<Exchange, ExchangeFeeConfig> configs_;

    // VIP 등급 테이블
    std::map<Exchange, std::vector<VipFeeLevel>> vip_tables_;

    // 스레드 안전
    mutable RWSpinLock mutex_;
};

// =============================================================================
// 글로벌 인스턴스 접근자
// =============================================================================
FeeCalculator& fee_calculator();

}  // namespace arbitrage
