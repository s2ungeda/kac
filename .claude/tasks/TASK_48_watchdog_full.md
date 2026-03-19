# TASK_48: Watchdog 전체 프로세스 오케스트레이션

> Phase 3

---

## 📌 목표

8개 프로세스 관리 (시작 순서, 감시, 재시작, 종료 순서)

---

## 🔧 프로세스 목록

```
Watchdog (PID 1)
├── upbit-feeder      (priority: 1)
├── bithumb-feeder    (priority: 1)
├── binance-feeder    (priority: 1)
├── mexc-feeder       (priority: 1)
├── arb-engine        (priority: 2)
├── order-manager     (priority: 3)
├── risk-manager      (priority: 3)
└── monitor           (priority: 4)
```

### 시작 순서

1. Feeders (SHM 세그먼트 생성)
2. arb-engine (SHM attach)
3. order-manager, risk-manager (병렬)
4. monitor

### 종료 순서 (역순)

1. monitor
2. order-manager, risk-manager
3. arb-engine
4. Feeders

---

## 📁 수정 파일

| 파일 | 변경 |
|------|------|
| `include/arbitrage/infra/watchdog.hpp` | 8 프로세스 관리 |
| `src/infra/watchdog.cpp` | 시작/종료 순서 구현 |

## 📎 의존성: TASK_40, TASK_47
## ⏱️ 예상: 1일
