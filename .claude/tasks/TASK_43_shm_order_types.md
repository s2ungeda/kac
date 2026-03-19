# TASK_43: SHM Order POD Types + Bidirectional Channel

> Phase 3

---

## 📌 목표

프로세스 간 주문 전달을 위한 POD 타입 정의

---

## 🔧 설계

### 문제

`DualOrderResult`의 `SingleOrderResult`에 `std::optional`, `std::string` 포함 → SHM 불가

### ShmDualOrderResult (POD)

```cpp
struct alignas(CACHE_LINE_SIZE) ShmDualOrderResult {
    bool buy_success;
    bool sell_success;
    OrderResult buy_result;   // 이미 POD (char[])
    OrderResult sell_result;  // 이미 POD (char[])
    int64_t total_latency_us;
    int64_t request_id;
    double actual_premium;
    double gross_profit;
    char error_message[256];
};
```

### OrderChannel

```cpp
class OrderChannel {
    ShmSPSCQueue<DualOrderRequest> request_queue;  // Engine → OrderManager
    ShmSPSCQueue<ShmDualOrderResult> result_queue;  // OrderManager → Engine
};
```

---

## 📁 생성 파일

| 파일 | 내용 |
|------|------|
| `include/arbitrage/ipc/ipc_types.hpp` | ShmDualOrderResult 추가 |
| `include/arbitrage/ipc/order_channel.hpp` | OrderChannel |

## 📎 의존성: TASK_36
## ⏱️ 예상: 0.5일
