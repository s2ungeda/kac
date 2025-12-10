# TASK 13: 동시 주문 실행기 (C++)

## 🎯 목표
아비트라지를 위한 두 거래소 동시 주문 실행

---

## ⚠️ 주의사항

```
절대 금지:
- 순차 실행 (매수 → 매도)
- 부분 체결 무시

필수:
- std::async 병렬 실행
- 둘 다 체결 확인
- 실패 시 복구 로직
```

---

## 📁 생성할 파일

```
include/arbitrage/executor/
├── dual_order.hpp
├── recovery.hpp
└── types.hpp
src/executor/
├── dual_order.cpp
└── recovery.cpp
```

---

## 📝 핵심 구현

### 1. 타입 정의

```cpp
// 동시 주문 요청
struct DualOrderRequest {
    OrderRequest buy_order;      // 매수 (해외)
    OrderRequest sell_order;     // 매도 (국내)
    double expected_premium;
    Duration buy_delay{0};       // RTT 보정
    Duration sell_delay{0};
};

// 개별 결과
struct SingleOrderResult {
    Exchange exchange;
    std::optional<OrderResult> result;
    std::optional<Error> error;
    Duration latency;
    
    bool is_success() const;
    bool is_filled() const;
    double filled_qty() const;
};

// 동시 주문 결과
struct DualOrderResult {
    SingleOrderResult buy_result;
    SingleOrderResult sell_result;
    SteadyTimePoint start_time;
    SteadyTimePoint end_time;
    
    bool both_success() const;
    bool both_filled() const;
    bool partial_fill() const;
    Duration total_latency() const;
};

// 복구 액션
enum class RecoveryAction {
    None,
    SellBought,     // 매수한 것 손절 매도
    BuySold,        // 매도한 것 매수 복구
    CancelBoth,
    ManualIntervention
};
```

### 2. 동시 실행기

```cpp
class DualOrderExecutor {
public:
    DualOrderExecutor(std::map<Exchange, std::shared_ptr<IExchange>> exchanges);
    
    // 동시 실행
    std::future<DualOrderResult> execute(const DualOrderRequest& request);
    DualOrderResult execute_sync(const DualOrderRequest& request);
    
private:
    SingleOrderResult execute_single(
        std::shared_ptr<IExchange> exchange,
        const OrderRequest& order,
        Duration delay
    );
};

DualOrderResult DualOrderExecutor::execute_sync(const DualOrderRequest& request) {
    DualOrderResult result;
    result.start_time = std::chrono::steady_clock::now();
    
    // ★ 핵심: std::async 병렬 실행
    auto buy_future = std::async(std::launch::async, [&]() {
        return execute_single(
            exchanges_[request.buy_order.exchange],
            request.buy_order,
            request.buy_delay
        );
    });
    
    auto sell_future = std::async(std::launch::async, [&]() {
        return execute_single(
            exchanges_[request.sell_order.exchange],
            request.sell_order,
            request.sell_delay
        );
    });
    
    // 결과 대기
    result.buy_result = buy_future.get();
    result.sell_result = sell_future.get();
    result.end_time = std::chrono::steady_clock::now();
    
    // 부분 체결 복구
    if (result.partial_fill() && recovery_) {
        auto plan = recovery_->create_plan(request, result);
        if (plan.action != RecoveryAction::None) {
            recovery_->execute_recovery(plan);
        }
    }
    
    return result;
}
```

### 3. 복구 관리자

```cpp
class RecoveryManager {
public:
    RecoveryPlan create_plan(
        const DualOrderRequest& request,
        const DualOrderResult& result
    );
    
    std::future<Result<OrderResult>> execute_recovery(const RecoveryPlan& plan);
};

RecoveryPlan RecoveryManager::create_plan(
    const DualOrderRequest& request,
    const DualOrderResult& result
) {
    bool buy_ok = result.buy_result.is_success();
    bool sell_ok = result.sell_result.is_success();
    
    if (buy_ok && sell_ok) {
        return {RecoveryAction::None};
    }
    
    if (buy_ok && !sell_ok) {
        // 매수 성공, 매도 실패 → 시장가 손절 매도
        return {
            RecoveryAction::SellBought,
            OrderRequest{
                .exchange = request.buy_order.exchange,
                .symbol = request.buy_order.symbol,
                .side = OrderSide::Sell,
                .type = OrderType::Market,
                .quantity = result.buy_result.filled_qty()
            },
            "Sell failed, liquidating bought position"
        };
    }
    
    if (!buy_ok && sell_ok) {
        // 매수 실패, 매도 성공 → 시장가 매수 복구
        return {
            RecoveryAction::BuySold,
            OrderRequest{
                .exchange = request.sell_order.exchange,
                .symbol = request.sell_order.symbol,
                .side = OrderSide::Buy,
                .type = OrderType::Market,
                .quantity = result.sell_result.filled_qty()
            },
            "Buy failed, covering sold position"
        };
    }
    
    // 둘 다 실패
    return {RecoveryAction::None, {}, "Both failed, no recovery"};
}
```

---

## ✅ 완료 조건

```
□ std::async 병렬 실행
□ 두 결과 수집
□ 부분 체결 감지
□ 복구 로직
□ 지연 측정
□ 통계 수집
```

---

## 📎 다음 태스크

완료 후: TASK_14_transfer.md
