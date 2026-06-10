# 중간 코드 리뷰 보고서 (2026-06-10)

> 49개 태스크 완료 시점의 전체 설계/코드 검토 결과와 1차 수정 내역.
> 4개 영역(동시성/IPC, 거래소 연동, 트레이딩 로직, 아키텍처) 병렬 분석 후 치명 이슈는 코드로 교차 검증함.

---

## 총평

설계(8프로세스 분리, Hot/Cold 경로, SHM IPC, Watchdog)는 합리적이고 일관성 있음.
다만 **실거래 투입 전 반드시 수정해야 할 문제**가 존재. 등급: **B+** — 골격은 좋으나
"돈이 걸린 경계 조건"(부분 체결, 스테일 데이터, 보안) 처리가 미흡.

---

## ✅ 1차 수정 완료 (이 보고서와 함께 커밋됨)

### 1. TLS 인증서 검증 활성화 (치명 → 해결)
- **문제**: 모든 거래소 WebSocket 연결에서 인증서 검증 비활성화 → MITM 공격으로 호가 조작/키 탈취 가능
- **수정**:
  - `src/feeder/feeder_process.cpp` — `verify_none` → `verify_peer`
  - `src/app/application.cpp` — 생성자에 `set_verify_mode(verify_peer)` 추가
  - `src/exchange/websocket_base.cpp` — `SSL_set1_host()`로 호스트네임 검증 추가 (SNI만으로는 검증 안 됨)
- **주의**: 시스템 CA 번들(`set_default_verify_paths`) 사용. 연결 실패 시 `ca-certificates` 패키지 확인 필요

### 2. TCP 제어 서버 인증 연결 (치명 → 해결)
- **문제**:
  - 인증 인프라(`require_auth=true`, AuthCallback)는 있었으나 **콜백을 등록하는 곳이 없음**
  - CLI는 `{"token":"..."}` JSON을 보내는데 서버는 `username:password`를 파싱 → **포맷 자체가 불일치**로 인증 불가능 상태
- **수정**:
  - `src/infra/tcp_client_manager.cpp` — 콜론 없는 payload는 토큰으로 간주 (password에 전달)
  - `tools/cli/commands.cpp` — JSON 래핑 제거, 토큰 원문 전송
  - `src/app/application.cpp`, `src/cold/monitor_process.cpp` — `config server.auth_token` 대조 콜백 등록
  - 토큰 미설정 시: 전체 거부 (fail-closed) + 시작 시 경고 로그
- **운영 필요**: `config.yaml`의 `server.auth_token` 설정, CLI `auth_token`과 일치시킬 것

### 3. 시그널 핸들러 async-signal-safe 재작성 (높음 → 해결)
- **문제**: 핸들러 안에서 `std::string` 생성(heap 할당) + `std::thread` 생성 — POSIX async-signal-safety 위반
- **수정** (`src/infra/shutdown.cpp`):
  - 핸들러는 lock-free atomic 플래그(`g_pending_signal`, `g_signal_seen`)만 설정
  - 두 번째 시그널은 `write()` + `_Exit()`만 사용 (둘 다 signal-safe)
  - 플래그 소비는 `wait_for_shutdown()`을 도는 일반 스레드가 수행
- **부수 수정**: `wait_for_shutdown(timeout)` 의미 교정
  - 기존: 시그널이 없어도 timeout(30초) 후 그냥 리턴 → **앱이 30초 뒤 무조건 종료되는 버그**
  - 변경: "종료 시작까지 무기한 대기(AdaptiveSpinWait) → 종료 완료까지 timeout 대기"
  - `shutdown_reason_` 쓰기에 lock 추가, `cancel_shutdown()` 시 시그널 플래그 리셋

### 검증
- 전체 빌드 성공
- `shutdown_test` 38/38, `tcp_server_test` 17/17 통과
- ctest 3/3 (integration, phase2, ipc) 통과

---

## 🔴 미해결 — 실거래 전 필수 (Priority 2)

| # | 이슈 | 위치 | 내용 |
|---|------|------|------|
| 1 | 레그 리스크: 부분 체결 처리 누락 | `src/executor/recovery.cpp:24-59` | 양쪽 모두 부분 체결 시(매수 50/매도 40) 둘 다 성공 판정 → 헤징 미스매치 방치. 매수 부분체결+매도 실패 시 체결분만 손절하고 잔여 미추적 |
| 2 | OrderTracker 영구 미완료 | `src/executor/order_tracker.cpp:227-258` | 완료 판정이 `ws_received && is_terminal()` — Private WS 끊기면 영원히 미완료. REST 폴링 폴백/타임아웃 필요 |
| 3 | FX 환율 스테일 캐시 5분 | `src/common/fxrate.cpp:223` | 크롤러 실패 시 5분 묵은 환율 사용. 김프 마진(1~2%) 대비 위험 → 스테일 시 거래 차단으로 변경 권장 |
| 4 | 주문 멱등성 부재 | `src/exchange/upbit/order.cpp:86-154` | 타임아웃 후 재시도 시 중복 주문 위험. `client_order_id` 기반 "조회 후 재시도" 패턴 필요 |
| 5 | 재연결 중 스테일 호가 | `src/exchange/websocket_base.cpp:334` | 재연결 동안 SHM의 옛 호가로 엔진이 판단 가능. 호가 age 체크 필요 |
| 6 | OrderManager 크래시 복구 | `src/cold/order_manager_main.cpp:53-116` | 주문 실행 중 사망 시 in-flight 주문 정합성 복구 경로 없음. 재시작 시 거래소 미체결 조회 → reconciliation 필요 |

## 🟠 미해결 — 안정화 (Priority 3)

| # | 이슈 | 위치 | 내용 |
|---|------|------|------|
| 7 | Detached 스레드 use-after-free | `src/app/application.cpp:599-659` | `std::thread([this]{...}).detach()` 4곳 — Application 소멸 후 멤버 접근 가능 |
| 8 | Memory Pool free-list ABA | `include/arbitrage/common/memory_pool.hpp:96-118` | CAS 스택의 전형적 ABA. tagged pointer 또는 thread-local freelist 권장 |
| 9 | Config 핫리로드 레이스 | `src/common/config_watcher.cpp:141-170` | `Config::instance().load()` 중 hot thread 동시 읽기. 스냅샷 교체(RCU식) 권장 |
| 10 | 일일 한도 race | `src/ops/daily_limit.cpp:78-99` | `can_trade()` → 주문 → `record_trade()` 사이 한도 초과 가능 |
| 11 | BasicArbStrategy 가격 단위 | `src/strategy/strategies/basic_arb_strategy.cpp:190` | KRW 환산가를 해외 주문 price에 사용. Market 주문이라 실주문엔 영향 없으나 DecisionEngine과 불일치 |
| 12 | UDS 부분 전송 미처리 | `src/ipc/unix_socket.cpp:309` | `sendmsg` 부분 전송 시 프레임 손실 |
| 13 | SpinLock 재진입 데드락 | `src/ipc/unix_socket.cpp:220-262` | lock 보유 중 콜백 호출 — 콜백이 broadcast하면 데드락 |
| 14 | tick/lot size 반올림 부재 | 주문 경로 전반 | 거래소별 호가단위/수량단위 반올림 로직 없음 |
| 15 | 최소 출금량 이중 관리 | transfer.cpp / fee_calculator.cpp | 두 곳의 min_withdraw 값 싱크 없음 |
| 16 | Service Locator 수명 | `include/arbitrage/app/service_locator.hpp` | raw pointer set/get — `std::atomic<T*>` + 소멸 시 해제 필요 |

## 🟡 권장 (지속)

- **단위 테스트 부재**: 김프 계산, 수수료, 주문 상태머신, recovery 분기 — 위 1·2번류 버그는 단위 테스트로만 잡힘
- Feeder 사망 후 SHM stale 데이터 (Ticker 타임스탬프 검증)
- API 키 heap wipe 강화 (JSON 객체 내 평문 잔존 — core dump 비활성화로 부분 완화됨)
- double 정밀도 (크립토 HFT 관행상 허용 범위이나, 저마진 누적 오차 인지할 것)

---

## 검토 중 확인된 오탐 (수정 불필요)

| 의심 | 결론 |
|------|------|
| "SPSC 큐에 4개 WS가 동시 push" | 4개 WS가 단일 `ioc_`/단일 io_thread에서 콜백 → single producer 유지. 단 io 스레드 증설 시 깨지는 암묵 계약 (주석 명시 권장) |
| "Binance depth 시퀀스 검증 누락" | `@depth{N}` 부분 스냅샷 스트림 사용 → `lastUpdateId` 관리 불필요 (diff 스트림 `@depth`만 해당) |
| "memory_pool pop_free 무한루프" | `compare_exchange_weak`가 실패 시 expected 자동 갱신 → 정상 |
| "Seqlock torn read 소비" | `load()`가 seq 불일치 시 false 반환 → 깨진 데이터 소비 안 됨 (엄밀히 non-atomic memcpy는 표준상 UB) |

---

## 이력

- 2026-06-10: 최초 리뷰 + Priority 1 수정 (TLS, TCP 인증, 시그널 핸들러)
