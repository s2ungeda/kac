# TASK 34: 오더북 분석기 (C++)

## 🎯 목표
실시간 오더북 분석 - 슬리피지 예측, 유동성 깊이 측정, Maker+Taker 최적 주문가 산출

---

## ⚠️ 핵심 개념

### Maker + Taker 전략
```
아비트라지 주문 구조:
- Maker: 지정가 주문 (호가창에 걸어둠) → 수수료 절감
- Taker: 시장가/즉시체결 → 확실한 체결

일반적 패턴:
1. 해외(매수): Maker 주문 (시간 여유, 수수료 절감)
2. 국내(매도): Taker 주문 (즉시 체결로 김프 확보)

또는 시장 상황에 따라 역방향 가능
```

### 분석 목적
```
1. 슬리피지 예측: X수량 체결 시 예상 평균가
2. 유동성 평가: 거래소별 호가 깊이 비교
3. 최적 Maker 가격: 체결 확률 vs 수수료 절감 trade-off
4. 실시간 모니터링: 급격한 유동성 변화 감지
```

---

## 📁 생성할 파일

```
include/arbitrage/strategy/
├── orderbook_analyzer.hpp
├── slippage_model.hpp
└── liquidity_metrics.hpp
src/strategy/
├── orderbook_analyzer.cpp
├── slippage_model.cpp
└── liquidity_metrics.cpp
tests/unit/strategy/
└── orderbook_analyzer_test.cpp
```

---

## 📝 핵심 구현

### 1. 유동성 메트릭 (liquidity_metrics.hpp)

```cpp
// 유동성 측정 결과
struct LiquidityMetrics {
    Exchange exchange;
    std::string symbol;
    
    // 스프레드
    double best_bid;
    double best_ask;
    double spread_bps;              // basis points (0.01%)
    
    // 깊이 (1% 범위 내 물량)
    double bid_depth_1pct;          // KRW 환산
    double ask_depth_1pct;
    
    // 불균형
    double imbalance;               // -1(매도벽) ~ +1(매수벽)
    
    // 시간
    std::chrono::system_clock::time_point timestamp;
};

// 호가별 상세
struct DepthLevel {
    double price;
    double quantity;
    double cumulative_qty;          // 누적 수량
    double cumulative_value_krw;    // 누적 금액
    double vwap;                    // 여기까지 VWAP
    double distance_pct;            // 최우선가 대비 거리(%)
};
```

### 2. 슬리피지 모델 (slippage_model.hpp)

```cpp
// 슬리피지 예측 결과
struct SlippageEstimate {
    double quantity;                // 주문 수량
    OrderSide side;                 // Buy/Sell
    
    double best_price;              // 최우선가
    double expected_avg_price;      // 예상 평균 체결가
    double worst_price;             // 최악 체결가 (마지막 레벨)
    double slippage_bps;            // 슬리피지 (bps)
    double slippage_krw;            // 슬리피지 (KRW)
    
    int levels_consumed;            // 소진되는 호가 레벨 수
    double fill_ratio;              // 체결 가능 비율 (0~1)
    bool fully_fillable;            // 전량 체결 가능 여부
    
    std::vector<DepthLevel> execution_path;  // 체결 경로
};

class SlippageModel {
public:
    // Taker 슬리피지 예측
    SlippageEstimate estimate_taker_slippage(
        const OrderBook& ob,
        OrderSide side,
        double quantity
    );
    
    // Maker 최적 가격 산출
    // - 체결 확률 target_fill_prob (0~1) 기준
    // - 과거 체결 데이터 기반
    double calculate_optimal_maker_price(
        const OrderBook& ob,
        OrderSide side,
        double target_fill_prob,
        Duration max_wait_time
    );
    
    // 과거 체결 데이터 피드 (Maker 가격 학습용)
    void feed_trade_data(const Trade& trade);
};
```

### 3. 오더북 분석기 (orderbook_analyzer.hpp)

```cpp
class OrderBookAnalyzer {
public:
    OrderBookAnalyzer(std::shared_ptr<FeeCalculator> fee_calc);
    
    // 오더북 업데이트
    void update(Exchange ex, const OrderBook& ob);
    
    // 유동성 메트릭 조회
    LiquidityMetrics get_liquidity(Exchange ex) const;
    std::map<Exchange, LiquidityMetrics> get_all_liquidity() const;
    
    // 슬리피지 예측
    SlippageEstimate estimate_slippage(
        Exchange ex,
        OrderSide side,
        double quantity
    );
    
    // ★ Maker+Taker 최적 주문 계획
    struct DualOrderPlan {
        // Maker 측
        Exchange maker_exchange;
        double maker_price;
        double maker_quantity;
        double maker_fee_rate;
        double expected_maker_fill_time_sec;
        
        // Taker 측
        Exchange taker_exchange;
        double taker_price;              // 예상 평균 체결가
        double taker_quantity;
        double taker_fee_rate;
        double taker_slippage_krw;
        
        // 총 비용
        double total_fee_krw;
        double total_slippage_krw;
        double net_premium_pct;          // 수수료+슬리피지 차감 후
        
        bool is_profitable() const;
    };
    
    DualOrderPlan plan_maker_taker_order(
        Exchange buy_ex,
        Exchange sell_ex,
        double quantity,
        double current_premium_pct
    );
    
    // 유동성 경고 콜백
    using LiquidityAlertCallback = std::function<void(Exchange, const std::string&)>;
    void on_liquidity_alert(LiquidityAlertCallback cb);
    
    // 설정
    void set_depth_threshold(double min_depth_krw);  // 최소 유동성 경고
    void set_spread_threshold(double max_spread_bps); // 스프레드 경고
    
private:
    std::map<Exchange, OrderBook> orderbooks_;
    std::map<Exchange, LiquidityMetrics> metrics_;
    std::shared_ptr<FeeCalculator> fee_calc_;
    std::shared_ptr<SlippageModel> slippage_model_;
    
    mutable std::shared_mutex mutex_;
};
```

---

## 📊 분석 기준값 (설정 가능)

```yaml
orderbook_analyzer:
  # 유동성 경고 임계값
  min_depth_krw: 50000000        # 5천만원 이하 시 경고
  max_spread_bps: 30             # 0.3% 이상 시 경고
  
  # Maker 가격 산출
  maker_fill_probability: 0.8   # 80% 체결 확률 목표
  maker_max_wait_sec: 30        # 최대 대기 시간
  
  # 슬리피지 계산 범위
  depth_levels: 20              # 분석할 호가 레벨 수
```

---

## 🔗 의존성

```
TASK_02~05: WebSocket (오더북 데이터 소스)
TASK_35: FeeCalculator (수수료 반영)
TASK_07: PremiumCalculator (순 프리미엄 계산)
```

---

## ✅ 완료 조건

```
□ 실시간 유동성 메트릭 계산
  □ 스프레드 (bps)
  □ 호가 깊이 (1% 범위)
  □ 매수/매도 불균형
  
□ 슬리피지 예측
  □ 수량별 예상 평균 체결가
  □ 체결 경로 (레벨별)
  □ 전량 체결 가능 여부
  
□ Maker+Taker 주문 계획
  □ Maker 최적 가격 산출
  □ 수수료+슬리피지 통합 계산
  □ 순 프리미엄 산출
  
□ 유동성 이상 감지
  □ 급격한 깊이 변화
  □ 스프레드 확대
  □ 콜백 알림

□ 스레드 안전 (shared_mutex)
□ 단위 테스트
```

---

## 📎 다음 태스크

완료 후: TASK_15_risk_model.md
