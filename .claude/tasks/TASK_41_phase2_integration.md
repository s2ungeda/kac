# TASK_41: Phase 2 Integration Test

> Phase 2 최종 검증

---

## 📌 목표

Feeder 분리 아키텍처 E2E 검증

---

## 🔧 테스트 시나리오

### 시나리오 1: 정상 운영
```bash
# Watchdog가 전체 시작
./watchdog --config config/phase2.yaml
# → 4 feeder + arb-engine 시작
# → 10분 안정 실행
# → Ctrl+C → 전체 종료
```

### 시나리오 2: Feeder 크래시 복구
```bash
# 실행 중 upbit-feeder kill
kill $(pgrep upbit-feeder)
# → Watchdog 2초 후 재시작
# → arb-engine 데이터 재수신 확인
```

### 시나리오 3: 성능 비교
```
Phase 1 (in-process): SPSC pop latency ~50ns
Phase 2 (SHM):        SHM pop latency ~100-200ns
차이 < 200ns → 거래소 API 100ms+ 대비 무의미
```

---

## ✅ 완료 조건

- [ ] 4 feeder + engine 동시 실행
- [ ] SHM 경유 시세 수신 확인
- [ ] Feeder 크래시 → 자동 재시작
- [ ] 10분 안정 실행
- [ ] 빌드 성공

## 📎 의존성: TASK_40
## ⏱️ 예상: 1일
