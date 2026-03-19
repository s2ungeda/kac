# 태스크 실행 순서 (29개)

> ⚠️ 이 순서대로 진행해야 의존성 문제가 발생하지 않습니다.

---

## 📊 실행 순서 요약

```
Phase 1 (기반)     : 01 → 02 → 03 → 04 → 05
Phase 2 (성능)     : 06 → 07
Phase 3 (거래)     : 08 → 09
Phase 4 (전략)     : 10 → 11 → 12 → 13 → 14
Phase 5 (인프라)   : 15 → 16 → 17 → 18 → 19 → 20
Phase 6 (서버)     : 21 → 22 → 23 → 24 → 25 → 26
Phase 7 (모니터링) : 27 → 28 → 29

총 29개 태스크
```

---

## 📋 상세 실행 순서

### Phase 1: 기반 구축 (5개) - 1주차

| 순서 | 태스크 | 파일명 | 설명 | 예상 시간 |
|:----:|--------|--------|------|:---------:|
| 1 | **TASK_01** | project_setup | CMake, 의존성, 기본 구조 | 1일 |
| 2 | **TASK_02** | websocket | 4개 거래소 WebSocket (통합) | 3일 |
| 3 | **TASK_03** | order_api | 4개 거래소 주문 API (통합) | 2일 |
| 4 | **TASK_04** | fxrate | 환율 조회 (USD/KRW) | 0.5일 |
| 5 | **TASK_05** | premium_matrix | 김프 계산 매트릭스 | 0.5일 |

**Phase 1 완료 시점 산출물:**
- 4개 거래소 실시간 시세 수신
- 4개 거래소 주문 API
- 김프 매트릭스 계산

---

### Phase 2: 성능 최적화 (2개) - 병렬 가능

| 순서 | 태스크 | 파일명 | 설명 | 예상 시간 |
|:----:|--------|--------|------|:---------:|
| 6 | **TASK_06** | lowlatency_infra | Lock-Free Queue + Memory Pool + Spin Wait | 2일 |
| 7 | **TASK_07** | rate_limiter_parser | Rate Limiter + simdjson | 1.5일 |

**Phase 2 완료 시점 산출물:**
- SPSC Queue, Object Pool
- Token Bucket Rate Limiter
- 초고속 JSON 파싱

---

### Phase 3: 거래 실행 (2개) - 2주차

| 순서 | 태스크 | 파일명 | 설명 | 예상 시간 |
|:----:|--------|--------|------|:---------:|
| 8 | **TASK_08** | executor | 동시 주문 실행기 | 1.5일 |
| 9 | **TASK_09** | transfer | 거래소 간 송금 | 1일 |

**Phase 3 완료 시점 산출물:**
- 동시 주문 실행
- 자동 송금

---

### Phase 4: 전략 (5개) - 3주차

| 순서 | 태스크 | 파일명 | 설명 | 예상 시간 |
|:----:|--------|--------|------|:---------:|
| 10 | **TASK_10** | orderbook_analyzer | 호가 분석, 슬리피지 예측 | 1일 |
| 11 | **TASK_11** | fee_calculator | 거래소별 수수료 계산 | 0.5일 |
| 12 | **TASK_12** | risk_model | 포지션 한도, VaR | 1.5일 |
| 13 | **TASK_13** | decision_engine | 진입/청산 결정, Kill Switch | 2일 |
| 14 | **TASK_14** | strategy_plugin | 전략 플러그인 시스템 | 1일 |

**Phase 4 완료 시점 산출물:**
- 완전한 아비트라지 전략 엔진
- 리스크 관리
- 플러그인 아키텍처

---

### Phase 5: 인프라 (6개) - 4주차

| 순서 | 태스크 | 파일명 | 설명 | 예상 시간 |
|:----:|--------|--------|------|:---------:|
| 15 | **TASK_15** | config_hotreload | 설정 핫리로드 | 0.5일 |
| 16 | **TASK_16** | secrets_manager | API 키 암호화 관리 | 1일 |
| 17 | **TASK_17** | multi_account | 다중 계정 지원 | 1일 |
| 18 | **TASK_18** | symbol_master | 심볼 매핑 관리 | 0.5일 |
| 19 | **TASK_19** | event_bus | 이벤트 버스 (pub/sub) | 1일 |
| 20 | **TASK_20** | thread_manager | 스레드 어피니티, 우선순위 | 1.5일 |

**Phase 5 완료 시점 산출물:**
- 보안 API 키 관리
- 다중 계정 운영
- 스레드 최적화

---

### Phase 6: 서버/운영 (6개) - 5주차

| 순서 | 태스크 | 파일명 | 설명 | 예상 시간 |
|:----:|--------|--------|------|:---------:|
| 21 | **TASK_21** | graceful_shutdown | 안전한 종료 | 0.5일 |
| 22 | **TASK_22** | health_check | 헬스 체크 엔드포인트 | 0.5일 |
| 23 | **TASK_23** | tcp_server | TCP 서버 + 프로토콜 | 1.5일 |
| 24 | **TASK_24** | alert_system | 알림 (텔레그램/Slack) | 1일 |
| 25 | **TASK_25** | daily_loss_limit | 일일 손실 한도 | 0.5일 |
| 26 | **TASK_26** | watchdog | 프로세스 감시/재시작 | 1일 |

**Phase 6 완료 시점 산출물:**
- 원격 모니터링
- 자동 복구
- 알림 시스템

---

### Phase 7: 모니터링/테스트 (3개) - 6주차

| 순서 | 태스크 | 파일명 | 설명 | 예상 시간 |
|:----:|--------|--------|------|:---------:|
| 27 | **TASK_27** | cli_tool | 명령줄 도구 | 1일 |
| 28 | **TASK_28** | trading_stats | 거래 통계 + Prometheus | 1.5일 |
| 29 | **TASK_29** | integration_test | 통합 테스트 | 2일 |

**Phase 7 완료 시점 산출물:**
- CLI 관리 도구
- 거래 통계/메트릭
- 전체 시스템 테스트

---

## 📊 의존성 그래프

```
TASK_01 (Project Setup)
    │
    ├─→ TASK_02 (WebSocket) ─────────────────┐
    │       │                                │
    ├─→ TASK_03 (Order API) ───────┐         │
    │       │                      │         │
    ├─→ TASK_04 (FX Rate) ───┐     │         │
    │                        │     │         │
    └─→ TASK_05 (Premium) ←──┴─────┤         │
                                   │         │
    TASK_06 (Low-Latency) ─────────┤         │
    TASK_07 (Rate Limiter) ────────┤         │
                                   ↓         │
                             TASK_08 (Executor)
                                   │         │
                                   ↓         │
    TASK_10~14 (전략) ←────────────┴─────────┘
         │
         ↓
    TASK_15~26 (인프라/서버)
         │
         ↓
    TASK_27~29 (모니터링/테스트)
```

---

## ⏱️ 총 예상 기간

| Phase | 태스크 수 | 예상 기간 |
|-------|:--------:|:--------:|
| Phase 1 (기반) | 5개 | 7일 |
| Phase 2 (성능) | 2개 | 3.5일 |
| Phase 3 (거래) | 2개 | 2.5일 |
| Phase 4 (전략) | 5개 | 6일 |
| Phase 5 (인프라) | 6개 | 5.5일 |
| Phase 6 (서버) | 6개 | 5일 |
| Phase 7 (모니터링) | 3개 | 4.5일 |
| **총계** | **29개** | **~7주** |

---

## 🔄 병렬 진행 가능 태스크

```
동시 진행 가능:
- TASK_06, 07 (성능) ↔ TASK_02, 03 (기반) 후반부
- TASK_15~19 (인프라 일부) ↔ TASK_10~14 (전략) 후반부
- TASK_27 (CLI) ↔ TASK_28 (Stats)
```

---

## 📎 파일 목록

```
TASK_01_project_setup.md      TASK_16_secrets_manager.md
TASK_02_websocket.md          TASK_17_multi_account.md
TASK_03_order_api.md          TASK_18_symbol_master.md
TASK_04_fxrate.md             TASK_19_event_bus.md
TASK_05_premium_matrix.md     TASK_20_thread_manager.md
TASK_06_lowlatency_infra.md   TASK_21_graceful_shutdown.md
TASK_07_rate_limiter_parser.md TASK_22_health_check.md
TASK_08_executor.md           TASK_23_tcp_server.md
TASK_09_transfer.md           TASK_24_alert_system.md
TASK_10_orderbook_analyzer.md TASK_25_daily_loss_limit.md
TASK_11_fee_calculator.md     TASK_26_watchdog.md
TASK_12_risk_model.md         TASK_27_cli_tool.md
TASK_13_decision_engine.md    TASK_28_trading_stats.md
TASK_14_strategy_plugin.md    TASK_29_integration_test.md
TASK_15_config_hotreload.md
```

---

## Phase 8: 모듈 통합 - Hot/Cold Thread Architecture (6개)

> Option A 변형: 단일 프로세스 내 Hot/Cold 스레드 분리
> 결정일: 2026-03-19

### 실행 순서

```
Phase 8 (통합): 30 → 31 → 32 → 35
                30 → 33 ──┘
                30 → 34 ──┘
```

| 순서 | 태스크 | 파일명 | 설명 | 예상 시간 |
|:----:|--------|--------|------|:---------:|
| 30 | **TASK_30** | cmake_fix_app_skeleton | CMake 수정 + App Skeleton | 2~3시간 |
| 31 | **TASK_31** | hot_cold_threads | SPSC Bridge + Hot Thread | 2~3시간 |
| 32 | **TASK_32** | order_execution_pipeline | Order Thread + Dry Run | 2~3시간 |
| 33 | **TASK_33** | cold_services | Cold 서비스 7개 통합 | 3~4시간 |
| 34 | **TASK_34** | utility_threads_shutdown | FX/Display Thread + Shutdown | 2시간 |
| 35 | **TASK_35** | e2e_dry_run | End-to-End Dry Run 테스트 | 1~2시간 |

### 의존성

```
TASK_30 (Skeleton)
    ├── TASK_31 (Hot Thread) → TASK_32 (Order) ─┐
    ├── TASK_33 (Cold Services) ────────────────┼── TASK_35 (E2E Test)
    └── TASK_34 (Utility + Shutdown) ───────────┘
```

### 병렬 가능

```
TASK_31, 33, 34는 TASK_30 완료 후 병렬 진행 가능
TASK_35는 모두 완료 후 실행
```

### 파일 목록

```
TASK_30_cmake_fix_app_skeleton.md
TASK_31_hot_cold_threads.md
TASK_32_order_execution_pipeline.md
TASK_33_cold_services.md
TASK_34_utility_threads_shutdown.md
TASK_35_e2e_dry_run.md
```
