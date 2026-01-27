# 프로젝트 진행 상황

> 이 파일은 Claude Code가 자동으로 업데이트합니다.
> 세션 시작 시 `/resume` 명령으로 이어서 작업할 수 있습니다.

---

## 📅 마지막 업데이트
- 날짜: 2026-01-27
- 세션: #11

---

## ✅ 완료된 태스크

| # | 태스크 | 완료일 | 테스트 | 비고 |
|---|--------|--------|--------|------|
| 01 | Project Setup | 2025-12-09 | ✅ 빌드 성공 | CMake, 기본 구조 완성 |
| 02 | WebSocket | 2025-12-09 | ⬜ Boost 없이 stub | WebSocket base, 4개 거래소 구현 |
| 03 | Order API | 2025-12-09 | ⬜ libcurl 없이 stub | REST API, Upbit/Binance 구현 |
| 04 | FXRate | 2026-01-15 | ✅ 빌드 성공 | USD/KRW 환율 (Selenium 크롤러 + 파일 기반) |
| 05 | Premium Matrix | 2026-01-15 | ✅ 빌드 성공 | 4x4 김프 매트릭스, 실시간 계산, 기회 감지 |
| 06 | Low Latency Infra | 2026-01-26 | ✅ 테스트 통과 | SPSC/MPSC Queue, Memory Pool, SpinWait/SpinLock |
| 07 | Rate Limiter + Parser | 2026-01-26 | ✅ 테스트 통과 | Token Bucket, RateLimitManager, 거래소별 JSON 파서 |
| 08 | Executor | 2026-01-27 | ✅ 테스트 통과 | DualOrderExecutor, RecoveryManager, std::async 병렬 실행 |
| 09 | Transfer | 2026-01-27 | ✅ 테스트 통과 | TransferManager, 출금/입금 관리, Destination Tag 처리 |

---

## 🔄 진행 중인 태스크

### 현재: 없음

다음 태스크: Phase 4 전략 구현 시작

---

## 📝 작업 로그

### 세션 #1 (2025-12-09)
- 17:00 - TASK_01 시작
- 17:10 - CMakeLists.txt 및 cmake 설정 파일 생성
- 17:12 - include/arbitrage 헤더 파일들 생성
- 17:15 - src/ 소스 파일들 생성 (main.cpp, config.cpp, logger.cpp)
- 17:18 - 의존성 없이도 빌드 가능하도록 수정 (spdlog 대신 간단한 로거 구현)
- 17:20 - 빌드 성공 및 실행 확인

### 세션 #2 (2025-12-09)
- 17:30 - TASK_02 WebSocket 시작
- 17:35 - WebSocket base class 및 Lock-Free Queue 구현
- 17:45 - 거래소별 WebSocket 클라이언트 구현 (Upbit, Binance, Bithumb, MEXC)
- 18:10 - JSON 파서 stub 구현 (nlohmann/json 대체)
- 18:30 - WebSocket 예제 파일 생성
- 18:40 - TASK_03 Order API 시작
- 18:45 - HTTP client 및 crypto 유틸리티 구현
- 18:50 - OrderClientBase 및 Upbit/Binance Order 클라이언트 구현
- 19:00 - Order API 예제 작성

### 세션 #3 (2025-12-09)
- 19:10 - TASK_04 FXRate 시작
- 19:15 - FXRateService 클래스 구현
- 19:20 - 환율 API 호출 및 캐싱 구현
- 19:25 - 자동 갱신 및 콜백 기능
- 19:30 - FXRate 예제 작성

### 세션 #4 (2025-12-10)
- TASK_05 Premium Matrix 완료
- PremiumCalculator 클래스 구현 (4x4 매트릭스)
- 실시간 김프 계산 및 기회 감지
- 임계값 기반 콜백 시스템
- Premium example 프로그램 작성 및 CMakeLists.txt 업데이트
- Phase 1 (기반 구축) 완료

### 세션 #5 (2025-12-18)
- TASK_04 FXRate 실제 구현
- Python Selenium 크롤러 개발 (investing.com 실시간 환율)
  - /scripts/fx_selenium_crawler.py - 메인 크롤러 (10초 주기)
  - /scripts/fx_watchdog.py - 크롤러 모니터링 및 자동 재시작
  - /tmp/usdkrw_rate.json - 환율 데이터 파일
- C++ FXRateService 수정
  - 파일 기반 환율 읽기로 변경
  - 데이터 신선도 체크 (30초)
  - Fallback 메커니즘 추가 (캐시 사용)
- 서비스 관리 스크립트
  - start_fx_service.sh / stop_fx_service.sh
- WebSocket 개선
  - Bithumb/MEXC WebSocket 구현
  - Protobuf 파서 추가 (MEXC용)
- 각종 테스트 프로그램 작성

### 세션 #11 (2026-01-27)
- TASK_09 Transfer 완료
  - TransferManager 클래스 구현 (include/arbitrage/executor/transfer.hpp)
    - 거래소 간 XRP 송금 관리
    - initiate(), initiate_sync(), initiate_with_callback() 지원
    - check_withdraw_status(), check_deposit() 상태 확인
    - wait_completion() 폴링 방식 완료 대기
  - 핵심 타입 정의
    - TransferRequest: 송금 요청 (from, to, amount, destination_tag)
    - TransferStatus: Pending, Processing, Completed, Failed, Timeout
    - TransferResult: 송금 결과 (transfer_id, tx_hash, elapsed)
    - WithdrawAddress: 입금 주소 정보 (화이트리스트 지원)
  - XRP 특화 기능
    - Destination Tag 필수 검증
    - 거래소별 출금 수수료 상수 (transfer_fees)
    - 거래소별 최소 출금 수량 검증
  - transfer_test 예제 프로그램 (8개 테스트 모두 통과)
    - Transfer Request Validation
    - Withdraw Fees
    - TransferManager Setup
    - Dry Run Transfer
    - Minimum Amount Check
    - Async Transfer
    - Statistics
    - Status Callback
- Phase 3 (거래) 완료!

### 세션 #10 (2026-01-27)
- TASK_08 Executor 완료
  - DualOrderExecutor 클래스 구현 (include/arbitrage/executor/dual_order.hpp)
    - std::async 병렬 실행으로 양방향 동시 주문
    - execute_sync(), execute_async(), execute_with_callback() 지원
    - 부분 체결 감지 및 자동 복구 트리거
    - ExecutorStats 통계 수집 (성공률, 지연시간, 복구율)
  - RecoveryManager 클래스 구현 (include/arbitrage/executor/recovery.hpp)
    - 복구 계획 자동 생성 (SellBought, BuySold, ManualIntervention)
    - 재시도 로직 (configurable max_retries, retry_delay)
    - dry_run 모드 지원 (테스트용)
    - RecoveryQueue 비동기 복구 큐
  - types.hpp 타입 정의
    - DualOrderRequest, SingleOrderResult, DualOrderResult
    - RecoveryAction, RecoveryPlan, RecoveryResult
    - ExecutorStats (atomic 기반 lock-free 통계)
  - executor_test 예제 프로그램 (9개 테스트 모두 통과)
    - Basic Dual Execution
    - Parallel Execution Verification (50ms 병렬 확인)
    - Partial Fill Detection
    - Recovery Plan Creation
    - Recovery Execution (Dry Run / Real)
    - Recovery Retry Logic
    - Auto Recovery Integration
    - Async Execution
- Phase 3 (거래) 50% 완료

### 세션 #9 (2026-01-26)
- 최적화 리뷰 및 과도한 부분 롤백
  - 유지된 좋은 부분:
    - alignas(64) 캐시 라인 정렬
    - types.hpp char[] + 헬퍼 함수 디자인
    - ObjectPool, SpinLock/SpinWait, ZeroCopyQueue
    - LIKELY/UNLIKELY, HOT_FUNCTION 매크로
  - 수정된 위험/과도한 부분:
    - memory_pool.hpp: 힙 fallback 복원 (안전성)
      - nullptr 대신 힙 할당으로 변경
      - heap_fallback_count() 모니터링 추가
    - compiler.hpp: 미사용 매크로 제거 (YAGNI)
      - 114줄 → 54줄 (-60줄)
      - PREFETCH_*, UNREACHABLE, ASSUME, RESTRICT 제거
    - fee_constants.hpp: 기본값 경고 추가
  - 테스트 결과
    - SPSC Queue: 1.0ns Push+Pop
    - Memory Pool: 2.3x 속도 향상 (16.4ns vs 37.4ns)
    - 모든 테스트 통과
  - 커밋: 1dfaae0 refactor: Restore heap fallback and remove unused macros

### 세션 #8 (2026-01-26)
- Low-Latency Core Optimization 완료
  - Cache-line alignment (alignas(64)) 적용
    - Ticker: 64 bytes = 1 cache line
    - OrderBook, OrderRequest, OrderResult, Balance: aligned
  - Zero-copy 지원
    - std::string → 고정 크기 char[] 변경
    - set_symbol(), set_order_id() 등 헬퍼 함수 추가
    - timestamp_us (int64_t 마이크로초) 사용
  - Memory Pool 힙 fallback 복원 (세션 #9에서 수정)
  - 전체 코드 업데이트
    - WebSocket 파서 (upbit, binance, bithumb, mexc)
    - Order 클라이언트 (upbit, binance)
    - Fast JSON Parser
    - 예제 프로그램 (lowlatency_test, order_example)
  - CLAUDE_CODE_RULES.md 저지연 가이드라인 추가
  - 테스트 결과
    - SPSC Queue: 0.8ns Push+Pop
    - Memory Pool: 2.6x 속도 향상 (16.8ns vs new/delete 43.9ns)
    - JSON Parser: 744,014 parses/sec
  - 커밋: 761535a refactor: Apply low-latency core optimization

### 세션 #7 (2026-01-26)
- TASK_07 Rate Limiter + Parser 완료
  - Token Bucket Rate Limiter (rate_limiter.hpp)
    - TokenBucketRateLimiter: Lock-Free 토큰 버킷
    - RateLimitManager: 거래소/API별 통합 관리
    - 거래소별 기본 Rate Limit 설정
  - Fast JSON Parser (fast_json_parser.hpp)
    - nlohmann/json 기반 (향후 simdjson 지원 예정)
    - 거래소별 파서: Upbit, Binance, Bithumb, MEXC
    - 640K parses/s 처리량
  - rate_limiter_test 예제 추가
- Phase 2 (성능 최적화) 완료

- TASK_06 Low Latency Infrastructure 완료
  - Data Logging 주석 처리 (main.cpp)
  - SPSC Queue 개선 + MPSC Queue 추가 (lockfree_queue.hpp)
  - Memory Pool 구현 (memory_pool.hpp)
    - FixedMemoryPool: Lock-Free 고정 크기 메모리 풀
    - ObjectPool<T>: 타입 안전 객체 풀
    - PoolAllocator: STL 호환 할당자
  - SpinWait/SpinLock 구현 (spin_wait.hpp)
    - SpinWait: CPU-친화적 대기
    - AdaptiveSpinWait: 적응형 스핀→yield→sleep
    - SpinLock: TTAS 패턴 락
    - RWSpinLock: Reader-Writer 락
    - ExponentialBackoff: 충돌 시 백오프
  - Pooled Types 정의 (pooled_types.hpp)
    - 글로벌 풀 접근자 (ticker_pool, orderbook_pool 등)
    - PooledPtr<T> RAII 래퍼
    - get_pool_stats() 통계 함수
  - lowlatency_test 예제 추가 및 테스트 통과
- Phase 2 (성능 최적화) 50% 완료

### 세션 #6 (2026-01-15)
- FXRate 빌드 에러 수정
  - Result<T> 템플릿에서 lvalue 참조 문제 해결
  - fxrate.cpp:226 - cached_rate_ 반환 시 복사본 생성
- TASK_04, TASK_05 상태 확인 및 업데이트
  - Premium Calculator 완전 구현 확인 (stub 아님)
  - 전체 빌드 성공 (100% 타겟 통과)
- 외부 라이브러리 설치 완료
  - nlohmann-json3-dev, libspdlog-dev, libyaml-cpp-dev 설치
- Selenium FX 크롤러 확인 및 시작
  - /scripts/fx_selenium_crawler.py 정상 동작
  - investing.com에서 실시간 환율 수신 (1468.29 KRW/USD)
- WebSocket 연결 문제 해결
  - Upbit: SNI 설정 추가로 해결 ✅
  - Bithumb: 정상 동작 ✅
  - Binance: 정상 동작 ✅
  - MEXC: aggre.deals.v3.api.pb 채널 형식으로 수정 ✅
- Premium Matrix 4개 거래소 모두 정상 동작
  - Upbit: 3111 KRW
  - Bithumb: 3112 KRW
  - Binance: 2.12 USDT
  - MEXC: 2.12 USDT
- bad_weak_ptr 크래시 수정
  - 소멸자에서 동기 방식으로 WebSocket 닫기
  - 타이머 취소 추가
  - 정상 종료 확인
- 실시간 데이터 로깅 기능 추가
  - examples/data_logger.cpp 작성
  - data/ 디렉토리에 데이터 저장
    - prices.csv: 가격 히스토리 (timestamp, exchange, symbol, price, currency)
    - premium.csv: 프리미엄 알림 (1% 이상)
    - fxrate.json: 현재 환율
    - summary.json: 최신 요약 (전체 가격 + 프리미엄 매트릭스)
  - 30초 테스트 성공 (151개 가격 레코드 기록)
- 메인 프로그램 (arbitrage) 기능 통합
  - src/main.cpp 전체 재작성
  - 4개 거래소 WebSocket 연결 (Upbit, Bithumb, Binance, MEXC)
  - 실시간 김프 매트릭스 계산 및 출력
  - 데이터 로깅 (prices.csv, premium.csv, fxrate.json, summary.json)
  - 환율 자동 갱신 (30초 주기)
  - Ctrl+C 시그널로 안전한 종료
  - Binance 소문자 심볼 처리 수정
- 파일 로깅 기능 추가
  - Logger 클래스에 날짜 기반 로그 파일 저장 기능 추가
  - logs/arbitrage_YYYY-MM-DD.log 형식
- Binance aggTrade 스트림으로 변경
  - @ticker → @aggTrade로 변경하여 실시간 체결 데이터 수신
  - is_trade() 메서드 추가 (WebSocketEvent)
- 4개 거래소 실시간 데이터 수신 확인
  - Upbit: Ticker (체결마다) ✅
  - Bithumb: Ticker (체결마다) ✅
  - Binance: Trade (aggTrade) ✅
  - MEXC: Trade (체결마다) ✅

---

## ⚠️ 알려진 이슈

- C++20 기능 중 일부가 GCC 9.4에서 제한적 (std::expected 미지원으로 Result 클래스 직접 구현)
- yaml-cpp CMake 설정 경고 (동작에는 문제 없음)

### ✅ 해결된 이슈 (2026-01-15)
- ~~Boost.Beast 미설치~~ → libboost-all-dev 설치됨
- ~~nlohmann/json 미설치~~ → nlohmann-json3-dev 설치됨
- ~~libcurl 미설치~~ → libcurl4-openssl-dev 설치됨
- ~~OpenSSL 미설치~~ → libssl-dev 설치됨
- ~~spdlog 미설치~~ → libspdlog-dev 설치됨
- ~~yaml-cpp 미설치~~ → libyaml-cpp-dev 설치됨
- ~~Upbit WebSocket 연결 안됨~~ → SNI 설정 추가로 해결
- ~~MEXC WebSocket 연결 안됨~~ → aggre.deals.v3.api.pb 채널 형식으로 해결
- ~~bad_weak_ptr 크래시~~ → 소멸자 동기 방식으로 수정

---

## 📌 다음 세션에서 할 일

1. Phase 4 시작 - 전략 구현
   - TASK_10: 기회 감지 (Opportunity Detector)
   - TASK_11: 리스크 관리 (Risk Manager)
   - TASK_12: Position Manager

2. 추가 저지연 최적화 (선택)
   - SIMD 가속 (simdjson 적용)
   - CPU Affinity (스레드 코어 고정)

---

## 🔧 개발 환경 상태

- OS: Linux (WSL2, Ubuntu 20.04)
- 컴파일러: g++ 9.4.0
- CMake: 3.16.3
- 빌드 상태: ✅ 성공
- 테스트 상태: ✅ lowlatency_test, rate_limiter_test, executor_test, transfer_test 통과

---

## 📊 전체 진행률

```
Phase 1 (기반):     ✅✅✅✅✅ 5/5 ✔️ 완료!
Phase 2 (성능):     ✅✅ 2/2 ✔️ 완료!
Phase 3 (거래):     ✅✅ 2/2 ✔️ 완료!
Phase 4 (전략):     ⬜⬜⬜⬜⬜ 0/5
Phase 5 (인프라):   ⬜⬜⬜⬜⬜⬜ 0/6
Phase 6 (서버):     ⬜⬜⬜⬜⬜⬜ 0/6
Phase 7 (모니터링): ⬜⬜⬜ 0/3

총 진행률: 9/29 (31.0%)
```

> ⚠️ 실행 순서는 TASK_ORDER.md 참조

---

## 📎 참고

- 규칙: [CLAUDE_CODE_RULES.md](./CLAUDE_CODE_RULES.md)
- 가이드: [CLAUDE_CODE_GUIDE.md](./CLAUDE_CODE_GUIDE.md)
- 순서: [TASK_ORDER.md](./TASK_ORDER.md)
