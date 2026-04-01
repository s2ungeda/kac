# TASK 13: 동시 주문 실행기 (C++)

## 🎯 목표
아비트라지를 위한 두 거래소 동시 주문 실행

---

## ⚠️ 주의사항

```
절대 금지:
- 순차 실행 (매수 → 매도)
- 부분 체결 무시
- 체결 확인을 REST 폴링으로 하기
- 주문 제출 후 응답 대기로 다음 주문을 블로킹하기

필수:
- 주문 제출은 fire-and-forget (제출 후 즉시 다음 주문 가능)
- 체결 확인은 Private WebSocket으로 비동기 수신 (TASK_02)
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
// Client Order ID 생성 규칙
// 형식: "arb_{request_id}_{side}" (예: "arb_1660801715431_buy", "arb_1660801715431_sell")
// - 제출 전에 생성 → OrderTracker에 등록 → 거래소에 전송
// - Private WS에서 수신 시 이 ID로 매칭
// - 매수/매도 쌍은 동일한 request_id를 공유
//
// ★ 역전 처리: Private WS 체결 통보가 REST 응답보다 먼저 올 수 있음
//   1. submit("arb_123_buy") → 거래소에 전송
//   2. 거래소 매칭엔진이 즉시 체결
//   3. Private WS: OrderUpdate(client_order_id="arb_123_buy", Filled) ← 먼저 도착
//   4. REST 응답: {orderId: 99999, status: "FILLED"} ← 나중에 도착 (또는 안 올 수도)
//
//   → client_order_id가 없으면 3번 시점에서 어떤 주문인지 매칭 불가
//   → client_order_id로 추적하면 REST 응답 순서와 무관하게 즉시 매칭 가능

// 동시 주문 요청
struct DualOrderRequest {
    OrderRequest buy_order;      // 매수 (해외) — client_order_id 포함
    OrderRequest sell_order;     // 매도 (국내) — client_order_id 포함
    double expected_premium;
    Duration buy_delay{0};       // RTT 보정
    Duration sell_delay{0};
    uint64_t request_id;         // 매수/매도 쌍을 묶는 ID (나노초 타임스탬프)
};

// 개별 결과
struct SingleOrderResult {
    Exchange exchange;
    std::string client_order_id; // 우리가 생성한 ID (추적 키)
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

```
현재 문제:
  기회 A → place_order() REST POST → HTTP 응답 대기(블로킹, 100ms+)
                                        → 그 사이 기회 B 발생 → 놓침
  1) 응답 대기 중 다음 주문을 못 냄
  2) 응답이 Open(접수됨)이면 이후 체결 완료를 감지할 방법 없음

변경 후:
  기회 A → client_order_id 생성 → submit() → 즉시 리턴 → 기회 B도 바로 처리 가능
                   ↓
           OrderTracker에 client_order_id 등록 (추적 시작)
                   ↓
           거래소에 전송 (client_order_id 포함)
                   ↓
  Private WS ←── OrderUpdate(client_order_id로 매칭) ←── 거래소
                   ↓
           OrderTracker가 매수/매도 쌍을 client_order_id로 집계
                   ↓
           체결/실패 시 콜백 → RecoveryManager / Stats 업데이트
```

```cpp
// 주문 제출: fire-and-forget, 즉시 리턴
class DualOrderExecutor {
public:
    DualOrderExecutor(std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients,
                      OrderTracker& tracker);

    // 주문 제출 — 비동기, 즉시 리턴
    // 1. client_order_id 생성 (매수: "arb_{request_id}_buy", 매도: "arb_{request_id}_sell")
    // 2. OrderTracker에 등록 (추적 시작)
    // 3. 거래소에 전송 (client_order_id 포함)
    // 4. 즉시 리턴 — 체결 결과는 Private WS → OrderTracker에서 비동기 수신
    void submit(const DualOrderRequest& request);

private:
    void submit_single(
        std::shared_ptr<OrderClientBase> client,
        const OrderRequest& order,
        Duration delay
    );

    OrderTracker& tracker_;
};

// 체결 추적: Private WS에서 수신한 OrderUpdate를 client_order_id로 매칭
class OrderTracker {
public:
    // 주문 쌍 등록 (submit 시 호출)
    // buy_client_id와 sell_client_id를 같은 request_id로 묶어서 관리
    void register_pair(uint64_t request_id,
                       const std::string& buy_client_id,
                       const std::string& sell_client_id,
                       const DualOrderRequest& request);

    // Private WS 콜백 — client_order_id로 어떤 주문인지 매칭
    void on_order_update(const OrderUpdate& update);

    // 매수/매도 양쪽 완료 시 콜백 → RecoveryManager, Stats
    using CompletionCallback = std::function<void(const DualOrderResult&)>;
    void on_completion(CompletionCallback cb);

    // 현재 미체결 주문 쌍 수
    size_t pending_count() const;
};

void DualOrderExecutor::submit(const DualOrderRequest& request) {
    // 1. client_order_id 생성
    auto buy_cid  = fmt::format("arb_{}_buy",  request.request_id);
    auto sell_cid = fmt::format("arb_{}_sell", request.request_id);

    // 2. OrderTracker에 쌍으로 등록 (Private WS 수신 전에 등록해야 함)
    tracker_.register_pair(request.request_id, buy_cid, sell_cid, request);

    // 3. client_order_id를 주문에 설정
    auto buy_order  = request.buy_order;
    auto sell_order = request.sell_order;
    std::memcpy(buy_order.client_order_id,  buy_cid.c_str(),  buy_cid.size());
    std::memcpy(sell_order.client_order_id, sell_cid.c_str(), sell_cid.size());

    // 4. 매수/매도 동시 전송 — 즉시 리턴
    std::thread([this, buy_order, request]() {
        submit_single(order_clients_[buy_order.exchange],
                      buy_order, request.buy_delay);
    }).detach();

    std::thread([this, sell_order, request]() {
        submit_single(order_clients_[sell_order.exchange],
                      sell_order, request.sell_delay);
    }).detach();
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
[x] std::async 병렬 실행
[x] 두 결과 수집
[x] 부분 체결 감지
[x] 복구 로직
[x] 지연 측정
[x] 통계 수집
[ ] submit()을 fire-and-forget으로 전환 (제출 후 즉시 리턴)
[ ] OrderTracker 구현 (Private WS OrderUpdate → order_id별 상태 집계)
[ ] completion callback (양쪽 체결 완료 시 → RecoveryManager / Stats)
[ ] 바이낸스: WS API 주문 제출 연동 (TASK_03)
[ ] 타임아웃 시 REST fallback (get_order)
```

---

## 📎 다음 태스크

완료 후: TASK_14_transfer.md
