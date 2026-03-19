# TASK_31: Hot/Cold Thread Architecture

> Phase 2 + Phase 3 통합

---

## 📌 목표

1. WebSocket IO 스레드 → SPSC Queue → Hot Thread 브릿지 구축
2. Hot Thread busy-poll 루프 구현 (코어 고정, 실시간 우선순위)

---

## 🔧 Phase 2: SPSC Bridge

### 현재
```
WebSocket callback → atomic<double> price 직접 업데이트 (IO 스레드에서)
```

### 변경 후
```
WebSocket callback → SPSCQueue<WebSocketEvent> push (IO 스레드)
                                    ↓
              Hot Thread busy-poll → pop → 처리
```

### 설계
- `SPSCQueue<WebSocketEvent>` 4096 슬롯
- Producer: IO 스레드 (io_context 단일 스레드, 4개 WebSocket 콜백 직렬화)
- Consumer: Hot Thread (단일)
- SPSC 조건 충족: Single Producer(IO) + Single Consumer(Hot)

### WebSocket 콜백 변경
```cpp
// Before
upbit_ws->on_event([&](const WebSocketEvent& evt) {
    double price = evt.ticker().price;
    price_upbit = price;
    calculator.update_price(Exchange::Upbit, price);
});

// After
upbit_ws->on_event([&ws_queue](const WebSocketEvent& evt) {
    ws_queue.push(evt);  // Fire-and-forget
});
```

---

## 🔧 Phase 3: Hot Thread

### 루프 구조
```cpp
void hot_thread_loop(
    SPSCQueue<WebSocketEvent>& ws_queue,
    PremiumCalculator& calculator,
    DecisionEngine& engine,
    SPSCQueue<DualOrderRequest>& order_queue,
    EventBus& event_bus,
    std::atomic<bool>& running
) {
    AdaptiveSpinWait waiter;

    while (running.load(std::memory_order_relaxed)) {
        bool had_work = false;
        WebSocketEvent event;

        // 1. SPSC Queue 드레인
        while (ws_queue.pop(event)) {
            had_work = true;

            if (event.is_ticker() || event.is_trade()) {
                // 2. 프리미엄 갱신
                calculator.update_price(event.exchange, event.ticker().price);

                // 3. Cold path 이벤트 발행 (non-blocking)
                event_bus.publish(events::TickerReceived{...});
            }
        }

        // 4. 기회 판단
        auto best = calculator.get_best_opportunity();
        if (best && best->premium_pct > min_threshold) {
            auto decision = engine.evaluate(*best);

            if (decision.should_execute()) {
                // 5. 주문 큐에 push (Order Thread가 처리)
                order_queue.push(decision.order_request);

                event_bus.publish(events::OpportunityDetected{...});
            }
        }

        // 6. Adaptive spin
        if (!had_work) {
            waiter.wait();
        } else {
            waiter.reset();
        }
    }
}
```

### ThreadManager 설정
```cpp
ThreadConfig hot_cfg;
hot_cfg.name = "hot_thread";
hot_cfg.core_id = 1;  // 코어 1 고정
hot_cfg.priority = ThreadPriority::RealTime;
```

### 핵심 제약
- Hot Thread 내 **블로킹 금지**: HTTP, 파일 IO, mutex lock 없음
- `event_bus.publish()`는 async 모드에서 queue push만 하므로 OK
- `calculator.update_price()`는 atomic store → OK
- `engine.evaluate()`는 atomic read + 간단한 계산 → OK

---

## 📁 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `src/main.cpp` | SPSC Queue 생성, WebSocket 콜백 변경, Hot Thread 함수 추가 |

---

## ✅ 완료 조건

- [ ] WebSocket 데이터가 SPSC Queue를 통해 Hot Thread로 전달
- [ ] Hot Thread가 busy-poll로 시세 수신 확인
- [ ] PremiumCalculator 실시간 갱신 동작
- [ ] DecisionEngine evaluate() 호출 확인 (로그)
- [ ] 코어 고정 + RealTime 우선순위 적용 확인
- [ ] 빌드 성공 + 실행 정상

---

## 📎 의존성

- TASK_30 (Application Skeleton)

## ⏱️ 예상 시간: 2~3시간
