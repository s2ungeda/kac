# TASK_40: Watchdog Multi-Process Management

> Phase 2

---

## 📌 목표

Watchdog가 4개 Feeder + arb-engine을 관리 (시작/감시/재시작)

---

## 🔧 설계

### 프로세스 관리

```
Watchdog
├── upbit-feeder     (자동 재시작)
├── bithumb-feeder   (자동 재시작)
├── binance-feeder   (자동 재시작)
├── mexc-feeder      (자동 재시작)
└── arb-engine       (자동 재시작)
```

### 시작 순서

1. Feeder 4개 시작 (SHM 세그먼트 생성)
2. 1초 대기 (SHM 준비)
3. arb-engine 시작 (SHM 세그먼트 attach)

### ChildProcessConfig

```cpp
struct ChildProcessConfig {
    std::string name;
    std::string executable;
    std::vector<std::string> arguments;
    int restart_delay_ms{2000};
    int max_restarts{10};
    bool critical{true};  // true면 영구 실패 시 전체 종료
};
```

---

## 📁 수정 파일

| 파일 | 변경 |
|------|------|
| `include/arbitrage/infra/watchdog.hpp` | ChildProcessConfig, 다중 프로세스 관리 |
| `src/infra/watchdog.cpp` | 구현 |

## 📎 의존성: TASK_38, TASK_39
## ⏱️ 예상: 1일
