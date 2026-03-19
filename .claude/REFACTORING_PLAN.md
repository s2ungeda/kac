# 아키텍처 리팩토링 계획 (검토 중)

> 작성일: 2026-03-16
> 결정일: 2026-03-19
> 상태: ✅ Option A 변형 채택 — 단일 프로세스 Hot/Cold 스레드 분리

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

## ✅ 결정 사항 (2026-03-19)

1. **분리 범위**
   - [x] **Option A 변형**: 단일 프로세스 내 Hot/Cold 스레드 분리
   - 거래소 API 지연 100ms+가 병목 → 프로세스 분리 ROI 낮음
   - 스레드 분리로 Hot/Cold 효과 동일 달성, 복잡도 최소

2. **IPC 방식**
   - [x] **In-process SPSC Queue** (50~100ns)
   - 프로세스 분리 없으므로 Shared Memory 불필요

3. **접근 방식**
   - [x] **점진적**: 모놀리식 완성 → 스레드 분리 → 필요 시 프로세스 분리
   - 실측 데이터 기반 최적화

### 구현 계획: TASK_30 ~ TASK_35 (6개 태스크)
- 상세: `.claude/tasks/TASK_30_*.md` ~ `TASK_35_*.md`
- 순서: `.claude/TASK_ORDER.md` Phase 8 섹션

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

## 📅 실행 계획

```
TASK_30: CMake 수정 + App Skeleton        ← 시작점
TASK_31: SPSC Bridge + Hot Thread
TASK_32: Order Execution Pipeline
TASK_33: Cold Services 7개 통합
TASK_34: Utility Threads + Shutdown
TASK_35: End-to-End Dry Run Test          ← 최종 검증
```

### 향후 (실거래 후 판단)
- Feed Handler 프로세스 분리 (Option A 본안)
- Cold Path 프로세스 분리 (Option C 부분 적용)
- 실측 병목 기반 추가 최적화
