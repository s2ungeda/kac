# TASK_33: Cold Services Integration

> Phase 5 전체

---

## 📌 목표

Cold Path 서비스들을 EventBus에 연결하고, ShutdownManager에 등록

---

## 🔧 서비스 목록

### 5a: HealthChecker
```
스레드: 자체 주기적 체크 스레드 (30초)
역할:
  - WebSocket 연결 상태 (4개)
  - DecisionEngine kill_switch 상태
  - SPSC Queue 사용률
  - CPU/메모리/FD 사용량
콜백:
  - on_unhealthy → AlertService 알림 + kill_switch 활성화
등록:
  - ShutdownManager priority: 500 (Storage)
```

### 5b: DailyLossLimiter
```
스레드: 자체 자정 리셋 타이머
역할:
  - 거래 손익 누적 추적
  - 일일 한도 도달 시 킬스위치
설정:
  - daily_loss_limit_krw (config에서 로드)
  - warning_threshold: 70%
  - critical_threshold: 90%
콜백:
  - on_warning → AlertService
  - set_kill_switch → DecisionEngine.set_kill_switch(true)
이벤트:
  - DailyLossLimitReached 발행
등록:
  - ShutdownManager priority: 500 (Storage)
```

### 5c: AlertService
```
스레드: 자체 async worker
역할:
  - Telegram/Discord/Slack 알림 전송
  - Rate limit 적용
구독 (EventBus):
  - KillSwitchActivated → critical 알림
  - DailyLossLimitReached → warning 알림
  - ExchangeDisconnected → error 알림
설정:
  - Telegram bot token + chat_id (config/secrets)
  - Discord webhook URL
등록:
  - ShutdownManager priority: 900 (Logging)
```

### 5d: TradingStatsTracker
```
스레드: auto-save 타이머 (60초)
역할:
  - 거래 통계 수집 (일/주/월/전체)
  - CSV 파일 저장
구독 (EventBus):
  - DualOrderCompleted → 거래 기록
등록:
  - ShutdownManager priority: 500 (Storage)
```

### 5e: ConfigWatcher
```
스레드: 자체 감시 스레드 (5초 주기)
역할:
  - config.yaml 변경 감지
  - Hot reload 시 컴포넌트 설정 업데이트
콜백:
  - on_reload → DecisionEngine 설정 갱신
  - on_reload → DailyLossLimiter 한도 갱신
이벤트:
  - ConfigReloaded 발행
등록:
  - ShutdownManager priority: 400 (Strategy)
```

### 5f: TcpServer
```
스레드: epoll 기반 서버 스레드
역할:
  - CLI 클라이언트 연결 수신
  - 상태 조회 응답
  - 킬스위치 제어 명령
메시지 핸들러:
  - CmdGetStatus → 시스템 상태 반환
  - CmdSetKillSwitch → DecisionEngine 제어
  - CmdStartStrategy / CmdStopStrategy
브로드캐스트:
  - PremiumUpdated, OpportunityDetected
설정:
  - port: 9090
  - auth_token (secrets)
등록:
  - ShutdownManager priority: 100 (Network)
```

### 5g: WatchdogClient
```
스레드: heartbeat 전송 스레드 (1초)
역할:
  - Watchdog 프로세스에 하트비트 전송
  - 명령 수신 (Shutdown, KillSwitch, ReloadConfig)
명령 핸들러:
  - Shutdown → ShutdownManager.initiate_shutdown()
  - KillSwitch → DecisionEngine.set_kill_switch()
  - ReloadConfig → ConfigWatcher.force_reload()
상태 업데이트:
  - HealthChecker 결과 → component_status bits
등록:
  - ShutdownManager priority: 100 (Network)
```

---

## 🔧 EventBus 구독 매핑

| 이벤트 | 구독자 |
|--------|--------|
| TickerReceived | TradingStatsTracker (시세 기록) |
| OpportunityDetected | AlertService (알림), TcpServer (브로드캐스트) |
| DualOrderCompleted | TradingStatsTracker, AlertService |
| KillSwitchActivated | AlertService (critical), TcpServer |
| DailyLossLimitReached | AlertService (warning) |
| ExchangeDisconnected | AlertService (error), HealthChecker |
| ConfigReloaded | TcpServer (브로드캐스트) |

---

## 📁 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `src/main.cpp` | 7개 Cold 서비스 초기화 + EventBus 구독 + ShutdownManager 등록 |

---

## ✅ 완료 조건

- [ ] 7개 서비스 모두 초기화 성공
- [ ] EventBus 구독 동작 확인 (이벤트 발행 → 핸들러 호출)
- [ ] ShutdownManager 등록 확인 (우선순위별)
- [ ] HealthChecker 주기적 체크 동작
- [ ] DailyLossLimiter 거래 기록 동작
- [ ] 빌드 성공

---

## 📎 의존성

- TASK_30 (Application Skeleton)
- TASK_32와 병렬 진행 가능

## ⏱️ 예상 시간: 3~4시간
