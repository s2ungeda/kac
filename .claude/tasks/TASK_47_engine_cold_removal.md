# TASK_47: arb-engine Cold Path 제거 + IPC 통합

> Phase 3

---

## 📌 목표

arb-engine에서 Cold 서비스 제거, IPC로 대체하여 경량화

---

## 🔧 제거 대상

| 제거 | 대체 |
|------|------|
| AlertService | → monitor 프로세스 |
| HealthChecker | → monitor 프로세스 |
| TradingStatsTracker | → monitor 프로세스 |
| TcpServer | → monitor 프로세스 |
| DailyLossLimiter | → risk-manager 프로세스 |
| Display Thread | → monitor 프로세스 |

### 유지

| 유지 | 이유 |
|------|------|
| Hot Thread | 핵심 로직 |
| FX Rate Thread | PremiumCalculator 직접 필요 |
| ConfigWatcher | 경량 + 엔진 설정 필요 |

### 추가

- Unix Socket Client → monitor, risk-manager에 이벤트 전송
- SHM Queue Producer → order-manager에 주문 전송
- SHM Queue Consumer → order-manager에서 결과 수신
- Unix Socket Server → risk-manager 킬스위치 명령 수신

---

## 📁 수정 파일

| 파일 | 변경 |
|------|------|
| `src/main.cpp` | Cold 서비스 제거, IPC 추가, --engine 모드 |

## 📎 의존성: TASK_44, TASK_45, TASK_46
## ⏱️ 예상: 1.5일
