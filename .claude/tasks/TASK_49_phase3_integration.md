# TASK_49: Phase 3 Integration Test

> Phase 3 최종 검증

---

## 📌 목표

8개 프로세스 전체 파이프라인 E2E 검증

---

## 🔧 테스트 시나리오

### 시나리오 1: 전체 시작/종료
```bash
./watchdog --config config/phase3.yaml
# → 8 프로세스 순서대로 시작
# → 10분 안정 실행
# → SIGTERM → 역순 종료
```

### 시나리오 2: 주문 파이프라인 (dry-run)
```
Feeder → SHM → Engine → SHM → OrderManager → 모의 체결
                                     ↓
                              RiskManager → P&L 기록
                              Monitor → 통계 표시
```

### 시나리오 3: 프로세스 크래시 복구
- order-manager kill → Watchdog 재시작 → 주문 재개
- feeder kill → Watchdog 재시작 → 데이터 재수신
- Engine 중단 없음 확인

---

## ✅ 완료 조건

- [ ] 8개 프로세스 동시 실행
- [ ] SHM + Unix Socket IPC 동작
- [ ] 프로세스 크래시 → 자동 재시작
- [ ] 10분 안정 실행
- [ ] Exit code 0

## 📎 의존성: TASK_48
## ⏱️ 예상: 1일
