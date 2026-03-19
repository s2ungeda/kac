# TASK_44: Order Manager Process

> Phase 3

---

## 📌 목표

주문 실행을 독립 프로세스로 분리

---

## 🔧 역할

```
SHM Queue (request) → DualOrderExecutor.execute_sync() → SHM Queue (result)
                                                        → Unix Socket (risk-manager)
```

### 기존 코드 이전

`main.cpp`의 `order_thread_func` 로직을 거의 그대로 이전:
- SHM Queue에서 DualOrderRequest pop
- DualOrderExecutor 실행
- SHM Queue에 결과 push (Engine feedback)
- Unix Socket으로 risk-manager에 거래 결과 전송

### 독립 실행

```bash
./order-manager --config config/config.yaml \
                --shm-request /kimchi_orders \
                --shm-result /kimchi_order_results \
                --risk-socket /tmp/kimchi_risk.sock
```

---

## 📁 생성 파일

| 파일 | 내용 |
|------|------|
| `include/arbitrage/cold/order_manager_process.hpp` | 클래스 |
| `src/cold/order_manager_process.cpp` | 구현 |
| `src/cold/order_manager_main.cpp` | main() |
| `src/cold/CMakeLists.txt` | 빌드 |

## 📎 의존성: TASK_42, TASK_43
## ⏱️ 예상: 1.5일
