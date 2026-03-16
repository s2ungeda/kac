# 아키텍처 리팩토링 계획 (검토 중)

> 작성일: 2026-03-16
> 상태: 🔄 검토 중 (내일 결정 예정)

---

## 📌 현재 상태

### 모놀리식 구조
```
┌─────────────────────────────────────────┐
│              arbitrage                   │
│  ┌─────┬─────┬─────┬─────┐             │
│  │Upbit│Bithm│Binan│MEXC │ ← WebSocket  │
│  └──┬──┴──┬──┴──┬──┴──┬──┘             │
│     └─────┴──┬──┴─────┘                 │
│              ↓                          │
│     ┌────────────────┐                  │
│     │ PremiumCalc    │                  │
│     │ DecisionEngine │                  │
│     │ OrderExecutor  │                  │
│     └────────────────┘                  │
└─────────────────────────────────────────┘
```

**문제점:**
- 하나가 죽으면 전체가 죽음
- 거래소 하나 재연결 시 전체 영향
- 코드 복잡도 증가

---

## 🎯 권장 리팩토링 모델

### Hot Path / Cold Path 분리

```
                        HOT PATH (단일 머신, Bare Metal)
┌─────────────────────────────────────────────────────────────────┐
│                                                                  │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐               │
│  │ Upbit   │ │ Bithumb │ │ Binance │ │  MEXC   │  Feed Handler │
│  │ Feeder  │ │ Feeder  │ │ Feeder  │ │ Feeder  │  (4 프로세스) │
│  └────┬────┘ └────┬────┘ └────┬────┘ └────┬────┘               │
│       │          │          │          │                        │
│       └──────────┴────┬─────┴──────────┘                        │
│                       ↓ Shared Memory (SPSC Queue)              │
│            ┌─────────────────────────┐                          │
│            │    Arbitrage Engine     │  ← 단일 프로세스         │
│            │  ┌─────────────────┐   │                          │
│            │  │ Premium Calc    │   │                          │
│            │  │ Decision Engine │   │                          │
│            │  └────────┬────────┘   │                          │
│            │           ↓            │                          │
│            │  ┌─────────────────┐   │                          │
│            │  │ Order Executor  │   │  ← 주문도 Hot Path!      │
│            │  │ (동시 2건 제출) │   │                          │
│            │  └─────────────────┘   │                          │
│            └─────────────────────────┘                          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
        │
        │ (비동기 이벤트)
        ↓
┌─────────────────────────────────────────────────────────────────┐
│                      WARM/COLD PATH                              │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │Order Manager │  │ Risk Manager │  │   Logger     │          │
│  │ (체결 추적)  │  │ (손익 계산)  │  │  (통계)      │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │  Telegram    │  │  Watchdog    │  │    CLI       │          │
│  │   알림       │  │   감시       │  │   도구       │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
└─────────────────────────────────────────────────────────────────┘
```

---

## 📊 프로세스 구성안

| 프로세스 | 역할 | Path | 비고 |
|----------|------|------|------|
| `upbit-feeder` | WebSocket 수신 + 파싱 | 🔥 Hot | 신규 |
| `bithumb-feeder` | WebSocket 수신 + 파싱 | 🔥 Hot | 신규 |
| `binance-feeder` | WebSocket 수신 + 파싱 | 🔥 Hot | 신규 |
| `mexc-feeder` | WebSocket 수신 + 파싱 | 🔥 Hot | 신규 |
| **`arb-engine`** | **전략 + 주문 실행** | 🔥 **Hot** | 기존 arbitrage 축소 |
| `order-manager` | 체결 추적, 부분 체결 | 🟡 Warm | 신규 |
| `risk-manager` | 포지션, 손익, 한도 | 🟡 Warm | 기존 모듈 분리 |
| `watchdog` | 프로세스 감시, 재시작 | ❄️ Cold | 기존 |
| `cli` | 관리 도구 | ❄️ Cold | 기존 |
| `alerter` | 텔레그램/슬랙 알림 | ❄️ Cold | 기존 모듈 분리 |

**총 10개 프로세스** (현재 5개 → 10개)

---

## 🔧 기술 스택

### 프로세스 간 통신 (IPC)
- **Shared Memory**: `shm_open()` + `mmap()`
- **Lock-Free Queue**: 이미 구현된 `SPSCQueue` 활용
- **지연 목표**: < 10μs

### 기존 활용 가능한 모듈
- `SPSCQueue` (include/arbitrage/common/lockfree_queue.hpp)
- `SpinLock` (include/arbitrage/common/spin_wait.hpp)
- `EventBus` (include/arbitrage/infra/event_bus.hpp)

---

## 🤔 결정 필요 사항

1. **분리 범위**
   - [ ] Option A: Feed Handler만 분리 (최소 변경)
   - [ ] Option B: Feed + Order Manager 분리
   - [ ] Option C: 전체 Hot/Cold 분리 (최대 변경)

2. **IPC 방식**
   - [ ] Shared Memory + SPSC Queue
   - [ ] Unix Domain Socket
   - [ ] TCP (localhost)

3. **우선순위**
   - [ ] 안정성 우선 → 분리
   - [ ] 속도 우선 → 현행 유지
   - [ ] 균형 → 점진적 분리

---

## 📚 참고 자료

### 해외 HFT 아키텍처
- **Hot Path / Cold Path 분리**: 실행은 베어메탈, 분석은 클라우드
- **Shared Memory**: 프로세스 간 Lock-Free 통신
- **Kernel Bypass**: DPDK, RDMA로 네트워크 지연 최소화
- **FPGA**: Jane Street, Citadel 등 사용

### 참고 링크
- [C++ Design Patterns for Low-Latency (arXiv)](https://arxiv.org/pdf/2309.04259)
- [Jane Street Performance Engineering](https://www.janestreet.com/performance-engineering/)
- [HFT Architecture 2026 Trends](https://www.tuvoc.com/blog/trading-system-architecture-microservices-agentic-mesh/)

---

## 📅 다음 단계

1. 위 결정 사항 검토
2. 리팩토링 범위 확정
3. 태스크 문서 작성 (TASK_30_*.md)
4. 점진적 구현

---

> 💡 이 문서는 검토용입니다. 최종 결정 후 수정됩니다.
