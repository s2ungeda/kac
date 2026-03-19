# TASK_30: CMake 수정 + Application Skeleton

> Phase 0 + Phase 1 통합

---

## 📌 목표

1. `ops` 라이브러리를 메인 실행 파일에 링크 (현재 누락)
2. `main.cpp`를 구조화된 초기화/실행/종료 흐름으로 재작성

---

## 🔧 Phase 0: CMake 수정

### 문제
`src/CMakeLists.txt` line 164-173에서 `arbitrage` 타겟에 `ops` 라이브러리 누락.
AlertService, DailyLossLimiter, TradingStatsTracker 사용 불가.

### 수정
```cmake
target_link_libraries(arbitrage
    PRIVATE
        common
        strategy
        executor
        infra
        ops          # ← 추가
        exchange_websocket
        exchange_order
        Threads::Threads
)
```

### 검증
`cmake --build build/ --target arbitrage` 성공

---

## 🔧 Phase 1: Application Skeleton

### main.cpp 초기화 순서

```
1.  Config::instance().load(config_path)
2.  Logger::init("logs")
3.  ShutdownManager::instance().install_signal_handlers()
4.  EventBus 생성 + async worker 시작 (2 threads)
5.  ShutdownManager에 EventBus 연결
6.  SymbolMaster::instance().init_xrp_defaults()
7.  FeeCalculator 초기화
8.  SecretsManager 초기화 (선택)
9.  AccountManager 설정 (선택)
10. ThreadManager::instance() 초기화
11. io_context, ssl_context, WebSocket 클라이언트 생성
12. OrderClient 생성 (Upbit, Binance)
13. RecoveryManager, DualOrderExecutor 생성
14. RiskModel, DecisionEngine 생성 (StrategyConfig 적용)
15. Cold 서비스 초기화 (Phase 5에서 상세 구현)
16. 모든 컴포넌트 ShutdownManager 등록
17. WebSocket 연결 시작
18. Hot Thread 시작 (Phase 2에서 구현)
19. ShutdownManager.wait_for_shutdown()
```

### 핵심 변경
- `std::signal()` → `ShutdownManager::install_signal_handlers()`
- `while (g_running) sleep(1s)` → `ShutdownManager::wait_for_shutdown()`
- 개별 `atomic<double>` → `PremiumCalculator`로 통합
- 수동 `disconnect()` → ShutdownManager 자동 순서 종료

### 코드 구조
```cpp
int main(int argc, char* argv[]) {
    // === 1. 설정 & 로거 ===
    // === 2. 시스템 컴포넌트 초기화 ===
    // === 3. 거래소 연결 ===
    // === 4. Hot/Cold 스레드 시작 ===
    // === 5. 종료 대기 ===
}
```

---

## 📁 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `src/CMakeLists.txt` | `ops` 라이브러리 링크 추가 |
| `src/main.cpp` | 전체 재작성 (Skeleton) |

---

## ✅ 완료 조건

- [ ] `ops` 라이브러리 링크 → 빌드 성공
- [ ] main.cpp에 모든 컴포넌트 #include
- [ ] 초기화 순서대로 컴포넌트 생성
- [ ] ShutdownManager로 시그널 처리
- [ ] 빌드 성공 + 실행 시 정상 시작/종료

---

## 📎 의존성

- 없음 (첫 번째 태스크)

## ⏱️ 예상 시간: 2~3시간
