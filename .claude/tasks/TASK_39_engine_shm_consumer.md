# TASK_39: arb-engine SHM Consumer

> Phase 2

---

## 📌 목표

arb-engine의 Hot Thread가 4개 SHM Queue에서 Ticker를 읽도록 변경

---

## 🔧 설계

### Hot Thread 변경

```cpp
// Before (Phase 1): 단일 in-process SPSC Queue
while (ws_queue.pop(event)) { ... }

// After (Phase 2): 4개 SHM Queue 라운드로빈
for (auto& [exchange, shm_queue] : shm_queues) {
    Ticker ticker;
    while (shm_queue.pop(ticker)) {
        calculator.update_price(exchange, ticker.price);
        // ...
    }
}
```

### 실행 모드

```bash
# Phase 1 (기존 — 단일 프로세스)
./arbitrage --standalone

# Phase 2 (SHM 모드 — Feeder가 별도 프로세스)
./arbitrage --engine
```

### --standalone 모드 유지

기존 WebSocket 직접 연결 + in-process SPSC Queue 방식 보존. 개발/테스트용.

---

## 📁 수정 파일

| 파일 | 변경 |
|------|------|
| `src/main.cpp` | --standalone/--engine 분기, SHM 4개 Queue 소비 |

## 📎 의존성: TASK_36, TASK_38
## ⏱️ 예상: 1일
