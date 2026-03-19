# TASK_46: Monitor Process

> Phase 3

---

## 📌 목표

모니터링/알림/통계를 독립 프로세스로 분리

---

## 🔧 역할

통합하는 기존 모듈:
- `AlertService` — Telegram/Discord/Slack 알림
- `TradingStatsTracker` — 거래 통계
- `HealthChecker` — 시스템 건강 모니터링
- `TcpServer` — CLI 클라이언트 연결
- Display — 콘솔 가격/프리미엄 출력

### IPC

- Unix Socket에서 이벤트 수신 (Engine, OrderManager)
- Unix Socket으로 건강 체크 ping (모든 프로세스)
- TcpServer로 외부 CLI 연결

---

## 📁 생성 파일

| 파일 | 내용 |
|------|------|
| `include/arbitrage/cold/monitor_process.hpp` | 클래스 |
| `src/cold/monitor_process.cpp` | 구현 |
| `src/cold/monitor_main.cpp` | main() |

## 📎 의존성: TASK_42
## ⏱️ 예상: 1.5일
