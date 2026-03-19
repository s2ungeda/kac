# TASK_34: Utility Threads + Graceful Shutdown

> Phase 6 + Phase 7 + Phase 8 통합

---

## 📌 목표

1. FX Rate 갱신 스레드 (Cold)
2. 상태 표시 스레드 (Cold, 현재 sleep 루프 대체)
3. Graceful Shutdown 전체 배선

---

## 🔧 FX Rate Thread

### 현재 문제
```cpp
// main loop 안에서 30초마다 blocking HTTP 호출
if (fx_elapsed >= 30) {
    auto fx = fx_service.fetch();  // ← 블로킹!
}
```

### 변경
```cpp
void fx_rate_thread(
    FXRateService& fx_service,
    PremiumCalculator& calculator,
    std::atomic<double>& current_fx_rate,
    std::atomic<bool>& running
) {
    while (running.load()) {
        auto fx = fx_service.fetch();
        if (fx) {
            current_fx_rate.store(fx.value().rate);
            calculator.update_fx_rate(fx.value().rate);
        }

        // 30초 대기 (condition_variable로 깨끗한 종료)
        std::unique_lock<std::mutex> lock(cv_mutex);
        cv.wait_for(lock, 30s, [&] { return !running.load(); });
    }
}
```

### ThreadManager 설정
```cpp
ThreadConfig fx_cfg;
fx_cfg.name = "fx_rate_thread";
fx_cfg.priority = ThreadPriority::Low;
// 코어 고정 불필요
```

---

## 🔧 Status Display Thread

### 현재의 sleep(1s) 루프 대체

```cpp
void display_thread(
    PremiumCalculator& calculator,
    /* 가격 atomics, 통계 참조 */
    std::atomic<bool>& running
) {
    int cycle = 0;

    while (running.load()) {
        cycle++;

        // 10초마다: 가격 요약
        if (cycle % 10 == 0) {
            print_prices(...);
        }

        // 30초마다: 프리미엄 매트릭스
        if (cycle % 30 == 0) {
            print_matrix(calculator.get_matrix());
        }

        // 60초마다: 거래 통계 + 헬스
        if (cycle % 60 == 0) {
            print_stats(...);
            print_health(...);
        }

        // 1초 대기
        std::unique_lock<std::mutex> lock(cv_mutex);
        cv.wait_for(lock, 1s, [&] { return !running.load(); });
    }
}
```

### ThreadManager 설정
```cpp
ThreadConfig display_cfg;
display_cfg.name = "display_thread";
display_cfg.priority = ThreadPriority::Idle;  // 최저 우선순위
```

---

## 🔧 Graceful Shutdown 배선

### 종료 우선순위

```
Priority 100 (Network):
  - WebSocket x4 disconnect
  - TcpServer stop
  - WatchdogClient disconnect

Priority 200 (Order):
  - DualOrderExecutor (진행 중 주문 대기)
  - Order queue drain

Priority 300 (Transfer):
  - RecoveryManager (진행 중 복구 완료)

Priority 400 (Strategy):
  - DecisionEngine cleanup
  - StrategyExecutor stop

Priority 500 (Storage):
  - TradingStatsTracker save (CSV)
  - DailyLossLimiter save
  - HealthChecker stop

Priority 900 (Logging):
  - AlertService flush (대기 중 알림 전송)
  - Logger::shutdown()
```

### ShutdownManager 등록 코드
```cpp
auto& shutdown = ShutdownManager::instance();

// Network (100)
shutdown.register_component("upbit_ws", [&]{ upbit_ws->disconnect(); },
                            ShutdownPriority::Network, 5s);
// ... (4개 WebSocket + TcpServer + WatchdogClient)

// Order (200)
shutdown.register_component("order_executor", [&]{
    // order_queue drain + 진행 중 주문 대기
    running = false;  // Hot/Order thread 중지
    order_thread.join();
}, ShutdownPriority::Order, 10s);

// Storage (500)
shutdown.register_component("trading_stats", [&]{
    stats_tracker.save_to_file("data/stats.csv");
}, ShutdownPriority::Storage, 5s);

// Logging (900)
shutdown.register_component("logger", [&]{
    alert_service.flush();
    Logger::shutdown();
}, ShutdownPriority::Logging, 3s);
```

### main() 종료 흐름
```cpp
// 이전: signal → g_running = false → 수동 disconnect
// 이후:
ShutdownManager::instance().wait_for_shutdown();
// ShutdownManager가 우선순위 순서대로 모든 컴포넌트 종료
```

---

## 📁 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `src/main.cpp` | FX thread, Display thread, Shutdown 등록 |

---

## ✅ 완료 조건

- [ ] FX Rate 30초 자동 갱신 동작
- [ ] 상태 표시 10초/30초/60초 주기 동작
- [ ] Ctrl+C → ShutdownManager 순서 종료
- [ ] 모든 스레드 깨끗한 join
- [ ] 종료 시 통계 파일 저장
- [ ] 빌드 성공 + 정상 시작/종료

---

## 📎 의존성

- TASK_30 (Application Skeleton)
- TASK_33 (Cold Services - Shutdown 등록 대상)

## ⏱️ 예상 시간: 2시간
