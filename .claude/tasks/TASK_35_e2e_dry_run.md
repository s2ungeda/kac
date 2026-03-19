# TASK_35: End-to-End Dry Run Test

> Phase 10

---

## 📌 목표

전체 파이프라인을 실 거래소 데이터 + 모의 주문으로 검증

---

## 🔧 Dry Run 모드

### CLI 플래그
```
./arbitrage --dry-run [--config config/config.yaml]
```

### 동작
```
실 거래소 WebSocket ─→ 시세 수신 (실제)
                       ↓
                 프리미엄 계산 (실제)
                       ↓
                 DecisionEngine (실제 판단)
                       ↓
                 DualOrderExecutor (dry-run: 로그만)
                       ↓
                 결과 기록 (모의 손익)
                       ↓
                 AlertService (실제 알림 가능)
```

### DualOrderExecutor dry-run
- `execute_sync()` 호출 시 실제 REST API 안 보냄
- 가상 체결 결과 생성 (현재가 기준)
- 지연 시뮬레이션 (100ms sleep)
- 로그 출력: "DRY RUN: Would buy X XRP on Binance @ Y"

---

## 🔧 검증 체크리스트

### 1. 데이터 흐름
```
[ ] WebSocket 4개 연결 성공
[ ] SPSC Queue로 시세 전달
[ ] Hot Thread에서 프리미엄 계산
[ ] DecisionEngine evaluate() 호출
[ ] 기회 감지 시 주문 큐 push
```

### 2. 주문 흐름
```
[ ] Order Thread에서 주문 수신
[ ] Dry-run 모드로 가상 체결
[ ] 거래 결과 기록 (DecisionEngine, DailyLossLimiter, TradingStats)
[ ] DualOrderCompleted 이벤트 발행
```

### 3. Cold 서비스
```
[ ] HealthChecker 주기적 체크
[ ] DailyLossLimiter 손익 누적
[ ] AlertService 이벤트 수신 (전송은 설정에 따라)
[ ] TradingStatsTracker 기록
[ ] ConfigWatcher 파일 감시
[ ] TcpServer CLI 연결 가능
```

### 4. 종료
```
[ ] Ctrl+C → 우선순위 순서 종료
[ ] 모든 스레드 join 완료
[ ] 통계 파일 저장
[ ] 메모리 누수 없음
```

### 5. 성능
```
[ ] Hot Thread CPU 사용률 확인
[ ] SPSC Queue 오버플로우 없음
[ ] WebSocket 재연결 동작
[ ] 10분 이상 안정 실행
```

---

## 🔧 테스트 시나리오

### 시나리오 1: 정상 운영 (10분)
```bash
./arbitrage --dry-run
# 10분 동안 실행
# 시세 수신, 프리미엄 계산, 기회 감지 확인
# Ctrl+C로 종료
```

### 시나리오 2: 킬스위치 테스트
```bash
# 터미널 1
./arbitrage --dry-run

# 터미널 2
./arbitrage-cli kill   # 킬스위치 활성화
./arbitrage-cli status # 상태 확인
./arbitrage-cli resume # 킬스위치 해제
```

### 시나리오 3: 설정 핫리로드
```bash
# 실행 중에 config.yaml 수정
# ConfigWatcher가 감지 → 설정 갱신 확인
```

---

## 📁 수정 파일

| 파일 | 변경 내용 |
|------|----------|
| `src/main.cpp` | --dry-run 플래그 파싱 및 적용 |

---

## ✅ 완료 조건

- [ ] --dry-run 모드로 10분 안정 실행
- [ ] 전체 파이프라인 데이터 흐름 확인
- [ ] Graceful Shutdown 정상 동작
- [ ] 통계 파일 생성 확인
- [ ] CLI 도구로 상태 조회 가능
- [ ] 빌드 성공

---

## 📎 의존성

- TASK_30, 31, 32, 33, 34 모두 완료 후

## ⏱️ 예상 시간: 1~2시간
