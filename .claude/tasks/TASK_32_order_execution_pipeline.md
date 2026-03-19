# TASK_32: Order Execution Pipeline

> Phase 4 + Phase 9 통합

---

## 📌 목표

1. Order Thread: SPSC Queue에서 주문 수신 → DualOrderExecutor 실행
2. StrategyExecutor를 Hot Thread와 통합 (내부 스레드 미사용)
3. 거래 결과 기록 (DecisionEngine, DailyLossLimiter, TradingStats)

---

## 🔧 Order Thread

### 설계
```
Hot Thread → SPSCQueue<DualOrderRequest> → Order Thread
                                              ↓
                                    DualOrderExecutor.execute_sync()
                                              ↓
                                    결과 기록 + 이벤트 발행
```

### 루프 구조
```cpp
void order_thread_loop(
    SPSCQueue<DualOrderRequest>& order_queue,
    DualOrderExecutor& executor,
    DecisionEngine& engine,
    DailyLossLimiter& daily_limiter,
    TradingStatsTracker& stats_tracker,
    EventBus& event_bus,
    std::atomic<bool>& running
) {
    AdaptiveSpinWait waiter;
    DualOrderRequest request;

    while (running.load(std::memory_order_relaxed)) {
        if (order_queue.pop(request)) {
            waiter.reset();

            // 주문 실행 (블로킹 OK - Cold path)
            auto result = executor.execute_sync(request);

            // 결과 기록
            double profit = calculate_profit(result);
            engine.record_trade_result(profit);
            daily_limiter.record_trade(profit);
            stats_tracker.record_trade(profit, result);

            // 이벤트 발행
            events::DualOrderCompleted evt;
            evt.success = result.both_filled();
            event_bus.publish(std::move(evt));

            // 쿨다운
            if (result.both_filled()) {
                engine.start_cooldown(/* config 값 */);
            }

            // 부분 체결 복구
            if (result.partial_fill()) {
                // RecoveryManager 처리
            }
        } else {
            waiter.wait();
        }
    }
}
```

### ThreadManager 설정
```cpp
ThreadConfig order_cfg;
order_cfg.name = "order_thread";
order_cfg.core_id = 2;  // 코어 2
order_cfg.priority = ThreadPriority::High;
```

---

## 🔧 StrategyExecutor 통합

### 결정: StrategyExecutor 내부 스레드 미사용

**이유:**
- StrategyExecutor의 `run_thread_`는 `eval_interval` (기본 100ms) 주기로 폴링
- Hot Thread가 직접 evaluate → 지연 없이 즉시 판단

### 통합 방식
```cpp
// StrategyExecutor.start() 호출 안 함 (내부 스레드 미시작)
// 대신 Hot Thread에서 직접:
strategy_executor.on_ticker_update(exchange, ticker);
strategy_executor.on_premium_update(matrix);
// evaluate는 DecisionEngine으로 직접
```

### BasicArbStrategy 활용
- StrategyExecutor에 BasicArbStrategy 등록
- 시장 데이터 업데이트는 Hot Thread에서 동기 호출
- 주문 결정은 DecisionEngine이 담당

---

## 🔧 Dry Run 모드

```cpp
// --dry-run 플래그
if (dry_run) {
    executor.set_dry_run(true);  // 실제 주문 안 보냄
    recovery.set_dry_run(true);
    logger->warn("DRY RUN MODE - No real orders will be placed");
}
```

---

## 📁 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `src/main.cpp` | Order Thread 함수, StrategyExecutor 초기화, dry-run 플래그 |

---

## ✅ 완료 조건

- [ ] Order Thread가 SPSC Queue에서 주문 수신
- [ ] DualOrderExecutor 호출 (dry-run 모드)
- [ ] 거래 결과 → DecisionEngine, DailyLossLimiter, TradingStats 기록
- [ ] DualOrderCompleted 이벤트 발행
- [ ] 부분 체결 시 RecoveryManager 트리거
- [ ] --dry-run 플래그 동작 확인
- [ ] 빌드 성공

---

## 📎 의존성

- TASK_31 (Hot Thread + SPSC Queue)

## ⏱️ 예상 시간: 2~3시간
