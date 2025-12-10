# TASK 35: 수수료 계산기 (C++)

## 🎯 목표
거래소별 정확한 수수료 계산 - Maker/Taker 구분, 출금 수수료, 네트워크 비용 통합 관리

---

## ⚠️ 중요 사항

### 수수료가 수익성에 미치는 영향
```
김프 3% 기회 발생 시 실제 수익:

거래 수수료:
- 해외 매수 (Maker): 0.02% 
- 국내 매도 (Taker): 0.05%
- 소계: 0.07%

출금 수수료:
- XRP 출금: 0.25 XRP ≈ 0.03% (1000 XRP 기준)

스프레드/슬리피지:
- 예상: 0.1~0.3%

실제 순수익: 3% - 0.07% - 0.03% - 0.2% ≈ 2.7%

→ 수수료 계산 오류 시 손실 가능!
```

### 수수료 구조 복잡성
```
1. 거래소마다 다른 수수료 체계
2. VIP 등급별 할인
3. 거래소 토큰 사용 시 할인 (BNB 등)
4. 이벤트성 수수료 면제
5. 출금 수수료 변동 (네트워크 혼잡도)
```

---

## 📁 생성할 파일

```
include/arbitrage/common/
└── fee_calculator.hpp
src/common/
└── fee_calculator.cpp
config/
└── fees.yaml
tests/unit/common/
└── fee_calculator_test.cpp
```

---

## 📝 핵심 구현

### 1. 수수료 타입 정의 (fee_calculator.hpp)

```cpp
#pragma once

#include "arbitrage/common/types.hpp"
#include <map>
#include <optional>

namespace arbitrage {

// 주문 유형
enum class OrderRole {
    Maker,      // 지정가 (호가 제공)
    Taker       // 시장가 (호가 소비)
};

// 거래소별 수수료 설정
struct ExchangeFeeConfig {
    Exchange exchange;
    
    // 거래 수수료 (%)
    double maker_fee_pct;           // Maker 수수료
    double taker_fee_pct;           // Taker 수수료
    
    // 할인 옵션
    double token_discount_pct;      // 거래소 토큰 사용 시 할인율
    bool use_token_for_fee;         // 토큰으로 수수료 지불 여부
    
    // VIP 등급 (0 = 일반)
    int vip_level;
    
    // 출금 수수료 (코인별)
    std::map<std::string, double> withdraw_fees;  // {"XRP": 0.25, "BTC": 0.0005}
    
    // 최소 출금 수량
    std::map<std::string, double> min_withdraw;
};

// 거래 비용 상세
struct TradeCost {
    double quantity;                // 거래 수량
    double price;                   // 거래 가격
    double notional_krw;            // 거래 금액 (KRW)
    
    OrderRole role;                 // Maker/Taker
    double fee_rate_pct;            // 적용 수수료율
    double fee_krw;                 // 수수료 (KRW)
    double fee_coin;                // 수수료 (코인, 토큰 지불 시)
    
    double net_quantity;            // 수수료 차감 후 수량
    double effective_price;         // 실효 가격 (수수료 반영)
};

// 송금 비용 상세
struct TransferCost {
    std::string coin;
    double amount;
    Exchange from;
    Exchange to;
    
    double withdraw_fee;            // 출금 수수료 (코인)
    double withdraw_fee_krw;        // 출금 수수료 (KRW 환산)
    double network_fee;             // 네트워크 수수료 (있을 경우)
    
    double net_amount;              // 실수령 수량
    double total_cost_krw;          // 총 비용 (KRW)
};

// 아비트라지 총 비용
struct ArbitrageCost {
    TradeCost buy_cost;             // 매수 비용
    TradeCost sell_cost;            // 매도 비용
    TransferCost transfer_cost;     // 송금 비용
    
    double total_fee_krw;           // 총 수수료
    double total_fee_pct;           // 총 수수료율 (%)
    
    double gross_profit_krw;        // 세전 수익 (김프)
    double net_profit_krw;          // 순수익
    double net_profit_pct;          // 순수익률 (%)
    
    bool is_profitable() const { return net_profit_krw > 0; }
};
```

### 2. 수수료 계산기 클래스

```cpp
class FeeCalculator {
public:
    FeeCalculator();
    
    // 설정 로드
    void load_config(const std::string& config_path);
    void set_exchange_config(const ExchangeFeeConfig& config);
    
    // 거래 수수료 계산
    TradeCost calculate_trade_cost(
        Exchange ex,
        OrderRole role,
        double quantity,
        double price,
        double fx_rate = 1.0        // USDT→KRW 환율
    ) const;
    
    // 송금 수수료 계산
    TransferCost calculate_transfer_cost(
        const std::string& coin,
        double amount,
        Exchange from,
        Exchange to,
        double coin_price_krw
    ) const;
    
    // ★ 아비트라지 총 비용 계산
    ArbitrageCost calculate_arbitrage_cost(
        Exchange buy_ex,
        Exchange sell_ex,
        double quantity,
        double buy_price,           // 원화 환산 전
        double sell_price,          // 원화
        double fx_rate,
        OrderRole buy_role,         // 보통 Maker
        OrderRole sell_role         // 보통 Taker
    ) const;
    
    // 손익분기 프리미엄 계산
    // 최소 몇 % 김프여야 수익인가?
    double calculate_breakeven_premium(
        Exchange buy_ex,
        Exchange sell_ex,
        OrderRole buy_role,
        OrderRole sell_role
    ) const;
    
    // 수수료율 조회
    double get_fee_rate(Exchange ex, OrderRole role) const;
    double get_withdraw_fee(Exchange ex, const std::string& coin) const;
    
    // VIP 등급 설정
    void set_vip_level(Exchange ex, int level);
    
    // 토큰 할인 설정
    void set_token_discount(Exchange ex, bool enabled);
    
    // 실시간 출금 수수료 업데이트 (API에서 가져올 경우)
    void update_withdraw_fee(Exchange ex, const std::string& coin, double fee);

private:
    std::map<Exchange, ExchangeFeeConfig> configs_;
    mutable std::shared_mutex mutex_;
    
    // VIP 등급별 수수료 테이블
    static const std::map<Exchange, std::vector<std::pair<double, double>>> VIP_FEE_TABLE;
};
```

---

## 📊 거래소별 기본 수수료 (2024년 기준, 변동 가능)

### fees.yaml

```yaml
exchanges:
  upbit:
    maker_fee_pct: 0.05
    taker_fee_pct: 0.05
    vip_levels: []                    # 업비트는 VIP 없음
    withdraw_fees:
      XRP: 0.0                        # 업비트 XRP 출금 무료
      BTC: 0.0005
    min_withdraw:
      XRP: 21
      BTC: 0.001
      
  bithumb:
    maker_fee_pct: 0.04
    taker_fee_pct: 0.04
    vip_levels:
      - level: 1
        maker: 0.035
        taker: 0.035
    withdraw_fees:
      XRP: 0.0                        # 빗썸 XRP 출금 무료
      BTC: 0.001
    min_withdraw:
      XRP: 20
      
  binance:
    maker_fee_pct: 0.10
    taker_fee_pct: 0.10
    bnb_discount_pct: 25              # BNB 결제 시 25% 할인
    vip_levels:
      - level: 1
        maker: 0.09
        taker: 0.10
      - level: 2
        maker: 0.08
        taker: 0.10
    withdraw_fees:
      XRP: 0.25
      BTC: 0.0002
    min_withdraw:
      XRP: 25
      
  mexc:
    maker_fee_pct: 0.00               # MEXC Maker 무료
    taker_fee_pct: 0.10
    mx_discount_pct: 20               # MX 토큰 20% 할인
    vip_levels: []
    withdraw_fees:
      XRP: 0.25
      BTC: 0.0003
    min_withdraw:
      XRP: 20

# 손익분기 계산 예시
# Binance(Maker) → Upbit(Taker) 아비트라지:
# - 매수 수수료: 0.075% (BNB 할인)
# - 매도 수수료: 0.05%
# - 출금 수수료: 0.25 XRP ≈ 0.025% (1000 XRP 기준)
# - 최소 필요 김프: 약 0.15% + 슬리피지
```

---

## 🔗 의존성

```
TASK_06: FX Rate (환율 정보)
TASK_19: Config Hot-reload (수수료 설정 갱신)
```

---

## ✅ 완료 조건

```
□ 거래소별 수수료 설정 로드
  □ YAML 파싱
  □ 런타임 갱신 지원

□ 거래 수수료 계산
  □ Maker/Taker 구분
  □ VIP 등급 반영
  □ 토큰 할인 반영
  □ KRW 환산
  
□ 출금 수수료 계산
  □ 코인별 수수료
  □ 실시간 업데이트 지원
  
□ 아비트라지 총 비용 계산
  □ 매수+매도+송금 통합
  □ 순수익 계산
  □ 손익분기 프리미엄 산출
  
□ 스레드 안전
□ 단위 테스트
  □ 각 거래소별 계산 검증
  □ 경계값 테스트
```

---

## 📎 다음 태스크

완료 후: TASK_34_orderbook_analyzer.md
