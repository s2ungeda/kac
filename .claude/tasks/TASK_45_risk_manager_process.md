# TASK_45: Risk Manager Process

> Phase 3

---

## 📌 목표

포지션/손익/한도 관리를 독립 프로세스로 분리

---

## 🔧 역할

- Unix Socket에서 거래 결과 수신 (order-manager)
- DailyLossLimiter 실행
- 포지션/잔고 추적
- 킬스위치 명령 → Engine (Unix Socket)
- 상태 영속화 (파일)

### 기존 모듈 활용

- `DailyLossLimiter` (ops/daily_limit.hpp)
- `RiskModel` (strategy/risk_model.hpp)

---

## 📁 생성 파일

| 파일 | 내용 |
|------|------|
| `include/arbitrage/cold/risk_manager_process.hpp` | 클래스 |
| `src/cold/risk_manager_process.cpp` | 구현 |
| `src/cold/risk_manager_main.cpp` | main() |

## 📎 의존성: TASK_42
## ⏱️ 예상: 1일
