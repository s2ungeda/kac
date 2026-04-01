# 주문 처리 흐름 (Order Processing Flow)

> **문서 버전**: v1.0
> **최종 수정**: 2026-04-01
> **변경 이력**:
> | 버전 | 날짜 | 내용 |
> |------|------|------|
> | v1.0 | 2026-04-01 | 최초 작성 |

---

## 개요

이 시스템은 **Hot Path**(전략 판단) → **Lock-Free Queue** → **Cold Path**(주문 실행)으로 분리된 구조입니다.
Hot Path에서 syscall/lock 없이 판단하고, Cold Path에서 네트워크 I/O를 분리 처리합니다.

```
WebSocket 호가 → DecisionEngine (프리미엄 판단)
                         │
                    DualOrderRequest
                         │ (SHM SPSC Queue, lock-free)
                         ↓
               OrderManager Process
                    │           │
              std::async    std::async
                    │           │
             바이낸스 매수   업비트 매도  (병렬 REST API)
                    │           │
                    └─────┬─────┘
                   DualOrderResult
                    │           │
              Engine 결과    Risk Manager
              (SHM 큐)      (UDS 소켓)
```

---

## 1. 전략 판단 (Hot Path, Core 0-1)

`DecisionEngine::evaluate()` (`src/strategy/decision_engine.cpp`)가 시장 데이터를 받아 판단합니다.

### 판단 순서

1. **전제 조건 검증** — kill switch, 쿨다운, 시장 상태 확인
2. **프리미엄 임계값 체크** — `OpportunityEvaluator`로 충분한 김프인지 판단
3. **리스크 평가** — `RiskModel`로 위험도 산정
4. **수량 최적화** — `QuantityOptimizer`로 최적 물량 계산
5. **`DualOrderRequest` 생성** — 해외 매수 + 국내 매도 주문을 하나로 묶음

### 주요 데이터 구조

**DualOrderRequest** (`include/arbitrage/executor/types.hpp`)
- cache-line aligned 구조체
- 매수 주문 (해외 거래소) + 매도 주문 (국내 거래소)
- 예상 프리미엄 (%)
- RTT 보정 딜레이 (buy_delay, sell_delay)
- request_id: 나노초 타임스탬프 자동 설정

**Decision 결과** (`include/arbitrage/strategy/decision_engine.hpp`)
- Decision enum: `Execute` / `Skip` / `Wait` / `HoldOff`
- DecisionReason enum: 15+ 사유 (InsufficientPremium, HighRiskScore 등)
- Execute일 경우 DualOrderRequest 포함

---

## 2. 주문 전달 (Lock-Free SPSC Queue)

결정된 주문은 **SPSC(Single Producer Single Consumer) 큐**를 통해 Cold Path로 전달됩니다.

### SHM 채널 구조 (`include/arbitrage/ipc/order_channel.hpp`)

| 방향 | SHM 이름 | 역할 |
|------|----------|------|
| Engine → OrderManager | `/kimchi_orders` | 주문 요청 큐 |
| OrderManager → Engine | `/kimchi_order_results` | 주문 결과 큐 |

### SHM 메모리 레이아웃 (`include/arbitrage/ipc/ipc_types.hpp`)

```
[ShmQueueHeader: 64B] [head: 64B] [tail: 64B] [buffer: capacity * element_size]
```

- **ShmQueueHeader** (64 bytes, cache-line aligned)
  - magic: `0xDEADBEEF4B494D43`
  - version, capacity (power of 2), element_size
  - producer_pid, consumer_pid, state (Init/Ready/Closed)
- **ShmAtomicIndex** (64 bytes, cache-line padded)
  - atomic head/tail 인덱스
- Lock-free, syscall 없음, POD 구조체 직접 기록

---

## 3. 주문 실행 (Cold Path, Core 2)

**OrderManager 프로세스** (`src/cold/order_manager_process.cpp`)가 SHM 큐를 spin-wait으로 폴링합니다.

### 실행 흐름

```
OrderThread (core 2, spin-wait polling)
    │
    ↓
DualOrderExecutor.execute_sync()
    │
    ├─ std::async → execute_single(buy)  → 해외 거래소 REST API
    │
    └─ std::async → execute_single(sell) → 국내 거래소 REST API
    │
    ↓
DualOrderResult (양쪽 결과 집계)
```

### 병렬 실행 (`src/executor/dual_order.cpp`)

1. **검증** — 교차 거래소 확인, 매수/매도 방향 확인, 수량 > 0
2. **`std::async`로 병렬 실행** — 두 거래소 API를 동시 호출
3. **RTT 기반 딜레이** — 느린 거래소가 먼저 전송되어 체결 시점 동기화
4. **양쪽 future 대기** → DualOrderResult 집계

### 거래소별 인증

| 거래소 | 인증 방식 | 엔드포인트 |
|--------|-----------|-----------|
| 업비트 | JWT (nonce + SHA512 해시 + HMAC-SHA256 서명) | `POST /v1/orders` |
| 바이낸스 | HMAC-SHA256 서명 + `X-MBX-APIKEY` 헤더 | `POST /api/v3/order` |

### 주문 데이터 구조

**OrderRequest** (`include/arbitrage/common/types.hpp`)
- 128 bytes (2 cache lines), cache-line aligned
- exchange, side (Buy/Sell), type (Limit/Market)
- symbol[16], quantity, price, client_order_id[48]

**OrderResult** (`include/arbitrage/common/types.hpp`)
- 256 bytes (4 cache lines)
- order_id[48], status, filled_qty, avg_price, commission
- timestamp_us, message[128]

---

## 4. 결과 처리

`DualOrderResult`에 양쪽 결과가 담겨 돌아오면:

1. **P&L 계산** — `gross_profit(fx_rate)`
2. **SHM 결과 큐로 push** — Engine에 결과 전달
3. **UDS로 risk-manager에 전송** — 리스크 관리 프로세스에 통보
4. **상태 업데이트** — 일일 손실 한도, 통계, 쿨다운 적용

### DualOrderResult 헬퍼 메서드 (`include/arbitrage/executor/types.hpp`)

- `both_success()` — 양쪽 모두 성공 여부
- `both_filled()` — 양쪽 모두 체결 여부
- `partial_fill()` — 부분 체결 여부
- `calculate_actual_premium(fx_rate)` — 실제 프리미엄 계산

---

## 5. 장애 복구 (Partial Fill)

한쪽만 체결되고 다른 쪽이 실패하면 `RecoveryManager` (`include/arbitrage/executor/recovery.hpp`)가 동작합니다.

### 복구 액션

| 상황 | RecoveryAction | 설명 |
|------|----------------|------|
| 해외 매수 성공 + 국내 매도 실패 | `SellBought` | 해외에서 손절 매도 |
| 국내 매도 성공 + 해외 매수 실패 | `BuySold` | 국내에서 복구 매수 |
| 양쪽 모두 실패 | `None` | 복구 불필요 |
| 복잡한 상황 | `ManualIntervention` | 수동 개입 필요 |

### 복구 설정

| 항목 | 기본값 |
|------|--------|
| 최대 재시도 | 3회 |
| 재시도 간격 | 100ms |
| 슬리피지 허용 | 0.5% |

---

## 6. 에러 처리

### Error 구조 (`include/arbitrage/common/error.hpp`)

`Result<T>` 템플릿 (variant 기반: `T | Error`)을 사용합니다.

### 에러 카테고리

| 카테고리 | 예시 |
|----------|------|
| Network | ConnectionFailed, Timeout, SSLError |
| API | AuthenticationFailed, RateLimited, InsufficientBalance |
| Internal | ParseError, InvalidState, ConfigError |
| Business | PremiumTooLow, RiskLimitExceeded, DailyLossLimitReached |

---

## 7. 관련 파일 목록

| 컴포넌트 | 경로 |
|----------|------|
| 공통 타입 | `include/arbitrage/common/types.hpp` |
| 에러 타입 | `include/arbitrage/common/error.hpp` |
| Decision Engine | `include/arbitrage/strategy/decision_engine.hpp`, `src/strategy/decision_engine.cpp` |
| Dual Order Executor | `include/arbitrage/executor/dual_order.hpp`, `src/executor/dual_order.cpp` |
| Executor 타입 | `include/arbitrage/executor/types.hpp` |
| Recovery Manager | `include/arbitrage/executor/recovery.hpp` |
| Order Base Interface | `include/arbitrage/exchange/order_base.hpp` |
| Upbit Order | `include/arbitrage/exchange/upbit/order.hpp`, `src/exchange/upbit/order.cpp` |
| Binance Order | `include/arbitrage/exchange/binance/order.hpp`, `src/exchange/binance/order.cpp` |
| Order Thread | `src/app/order_thread.cpp` |
| OrderManager Process | `include/arbitrage/cold/order_manager_process.hpp`, `src/cold/order_manager_process.cpp` |
| IPC Order Channel | `include/arbitrage/ipc/order_channel.hpp` |
| IPC 타입 | `include/arbitrage/ipc/ipc_types.hpp` |
