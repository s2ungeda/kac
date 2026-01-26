#pragma once

/**
 * 거래소별 수수료 상수 (Constexpr)
 *
 * 컴파일 타임에 수수료율 계산 가능
 * 런타임 오버헤드 제로
 *
 * ⚠️ 주의: 기본값입니다!
 * - 실제 수수료는 VIP 등급, 쿠폰, 이벤트에 따라 다를 수 있음
 * - 운영 환경에서는 설정 파일이나 API에서 동적으로 로드 권장
 * - 이 값은 손익분기점 계산의 보수적 기준값으로 사용
 */

#include "arbitrage/common/types.hpp"
#include <array>
#include <cstdint>

namespace arbitrage {

// =============================================================================
// 수수료율 (기본값, VIP 등급에 따라 다를 수 있음)
// =============================================================================

namespace fee {

// 거래소 수 (Exchange::Count)
constexpr size_t EXCHANGE_COUNT = static_cast<size_t>(Exchange::Count);

// Maker 수수료율 (지정가 주문)
constexpr std::array<double, EXCHANGE_COUNT> MAKER_FEE = {
    0.0005,   // Upbit:   0.05%
    0.0004,   // Bithumb: 0.04% (쿠폰 적용 시)
    0.0010,   // Binance: 0.10% (BNB 결제 시 0.075%)
    0.0000    // MEXC:    0.00% (Maker 무료)
};

// Taker 수수료율 (시장가 주문)
constexpr std::array<double, EXCHANGE_COUNT> TAKER_FEE = {
    0.0005,   // Upbit:   0.05%
    0.0004,   // Bithumb: 0.04%
    0.0010,   // Binance: 0.10%
    0.0002    // MEXC:    0.02%
};

// 출금 수수료 (XRP 기준, 코인 수량)
constexpr std::array<double, EXCHANGE_COUNT> WITHDRAW_FEE_XRP = {
    1.0,      // Upbit:   1 XRP
    1.0,      // Bithumb: 1 XRP
    0.25,     // Binance: 0.25 XRP
    0.25      // MEXC:    0.25 XRP
};

// =============================================================================
// Constexpr 수수료 조회 함수
// =============================================================================

/**
 * Maker 수수료율 조회
 * @param ex 거래소
 * @return 수수료율 (예: 0.0005 = 0.05%)
 */
constexpr double maker_fee(Exchange ex) noexcept {
    return MAKER_FEE[static_cast<size_t>(ex)];
}

/**
 * Taker 수수료율 조회
 * @param ex 거래소
 * @return 수수료율
 */
constexpr double taker_fee(Exchange ex) noexcept {
    return TAKER_FEE[static_cast<size_t>(ex)];
}

/**
 * 수수료율 조회 (Maker/Taker 통합)
 * @param ex 거래소
 * @param is_maker Maker 주문 여부
 * @return 수수료율
 */
constexpr double get_fee(Exchange ex, bool is_maker) noexcept {
    return is_maker ? maker_fee(ex) : taker_fee(ex);
}

/**
 * XRP 출금 수수료 조회
 * @param ex 거래소
 * @return 출금 수수료 (XRP 수량)
 */
constexpr double withdraw_fee_xrp(Exchange ex) noexcept {
    return WITHDRAW_FEE_XRP[static_cast<size_t>(ex)];
}

// =============================================================================
// 수수료 계산 함수 (Constexpr)
// =============================================================================

/**
 * 거래 수수료 계산
 * @param amount 거래 금액
 * @param ex 거래소
 * @param is_maker Maker 주문 여부
 * @return 수수료 금액
 */
constexpr double calculate_fee(double amount, Exchange ex, bool is_maker) noexcept {
    return amount * get_fee(ex, is_maker);
}

/**
 * 수수료 차감 후 금액 계산
 * @param amount 원래 금액
 * @param ex 거래소
 * @param is_maker Maker 주문 여부
 * @return 수수료 차감 후 금액
 */
constexpr double after_fee(double amount, Exchange ex, bool is_maker) noexcept {
    return amount * (1.0 - get_fee(ex, is_maker));
}

// =============================================================================
// 아비트라지 손익분기점 계산 (Constexpr)
// =============================================================================

/**
 * 왕복 수수료율 계산 (매수 + 매도 + 출금)
 * @param buy_ex 매수 거래소
 * @param sell_ex 매도 거래소
 * @return 총 수수료율 (예: 0.0015 = 0.15%)
 */
constexpr double round_trip_fee(Exchange buy_ex, Exchange sell_ex) noexcept {
    return taker_fee(buy_ex) + taker_fee(sell_ex);
}

/**
 * 손익분기 프리미엄 계산
 * 이 프리미엄 이상이어야 수익 발생
 * @param buy_ex 매수 거래소
 * @param sell_ex 매도 거래소
 * @return 최소 필요 프리미엄 (예: 0.002 = 0.2%)
 */
constexpr double breakeven_premium(Exchange buy_ex, Exchange sell_ex) noexcept {
    // 수수료 + 안전 마진 (슬리피지 등)
    constexpr double SAFETY_MARGIN = 0.001;  // 0.1%
    return round_trip_fee(buy_ex, sell_ex) + SAFETY_MARGIN;
}

// =============================================================================
// 프리미엄 임계값 (Constexpr)
// =============================================================================

// 기본 프리미엄 임계값
constexpr double PREMIUM_THRESHOLD_DEFAULT = 0.02;    // 2%

// 공격적 프리미엄 임계값
constexpr double PREMIUM_THRESHOLD_AGGRESSIVE = 0.015; // 1.5%

// 보수적 프리미엄 임계값
constexpr double PREMIUM_THRESHOLD_CONSERVATIVE = 0.03; // 3%

/**
 * 거래소 쌍별 최적 프리미엄 임계값 (Constexpr)
 * 손익분기점 + 목표 수익률
 */
constexpr double optimal_threshold(Exchange buy_ex, Exchange sell_ex) noexcept {
    constexpr double TARGET_PROFIT = 0.005;  // 0.5% 목표 수익
    return breakeven_premium(buy_ex, sell_ex) + TARGET_PROFIT;
}

}  // namespace fee

// =============================================================================
// 컴파일 타임 검증 (Static Assert)
// =============================================================================

// 배열 크기 검증
static_assert(fee::MAKER_FEE.size() == fee::EXCHANGE_COUNT,
              "MAKER_FEE array size mismatch");
static_assert(fee::TAKER_FEE.size() == fee::EXCHANGE_COUNT,
              "TAKER_FEE array size mismatch");
static_assert(fee::WITHDRAW_FEE_XRP.size() == fee::EXCHANGE_COUNT,
              "WITHDRAW_FEE_XRP array size mismatch");

// 수수료율 범위 검증 (0 ~ 1%)
static_assert(fee::maker_fee(Exchange::Upbit) >= 0.0 &&
              fee::maker_fee(Exchange::Upbit) <= 0.01,
              "Upbit maker fee out of range");
static_assert(fee::taker_fee(Exchange::Binance) >= 0.0 &&
              fee::taker_fee(Exchange::Binance) <= 0.01,
              "Binance taker fee out of range");

}  // namespace arbitrage
