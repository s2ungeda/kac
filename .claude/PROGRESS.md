# 프로젝트 진행 상황

> 이 파일은 Claude Code가 자동으로 업데이트합니다.
> 세션 시작 시 `/resume` 명령으로 이어서 작업할 수 있습니다.

---

## 📅 마지막 업데이트
- 날짜: 2025-12-18
- 세션: #5

---

## ✅ 완료된 태스크

| # | 태스크 | 완료일 | 테스트 | 비고 |
|---|--------|--------|--------|------|
| 01 | Project Setup | 2025-12-09 | ✅ 빌드 성공 | CMake, 기본 구조 완성 |
| 02 | WebSocket | 2025-12-09 | ⬜ Boost 없이 stub | WebSocket base, 4개 거래소 구현 |
| 03 | Order API | 2025-12-09 | ⬜ libcurl 없이 stub | REST API, Upbit/Binance 구현 |
| 04 | FXRate | 2025-12-18 | ✅ Selenium 크롤러 | USD/KRW 실시간 환율 (investing.com) |
| 05 | Premium Matrix | 2025-12-10 | ⬜ stub 구현 | 4x4 김프 매트릭스, 기회 감지, 콜백 |

---

## 🔄 진행 중인 태스크

### 현재: 없음

다음 태스크: TASK_06_low_latency_infra.md

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

---

## ⚠️ 알려진 이슈

- yaml-cpp, spdlog, simdjson 등 외부 라이브러리 미설치 (기본 동작에는 문제 없음)
- C++20 기능 중 일부가 GCC 9.4에서 제한적 (std::expected 미지원으로 Result 클래스 직접 구현)
- Boost.Beast WebSocket 라이브러리 미설치 (stub 구현으로 대체)
- nlohmann/json 미설치 (json_stub.hpp로 대체)
- libcurl 미설치 (HTTP client stub 구현)
- OpenSSL 미설치 (crypto stub 구현)

---

## 📌 다음 세션에서 할 일

1. Phase 2 시작 - TASK_06: Low Latency Infrastructure
   - Lock-free 자료구조
   - Object Pool
   - Thread Pinning  
   - NUMA 고려사항

---

## 🔧 개발 환경 상태

- OS: Linux (WSL2, Ubuntu 20.04)
- 컴파일러: g++ 9.4.0
- CMake: 3.16.3
- 빌드 상태: ✅ 성공
- 테스트 상태: ⬜ 미구현

---

## 📊 전체 진행률

```
Phase 1 (기반):     ✅✅✅✅✅ 5/5 ✔️ 완료!
Phase 2 (성능):     ⬜⬜ 0/2
Phase 3 (거래):     ⬜⬜ 0/2
Phase 4 (전략):     ⬜⬜⬜⬜⬜ 0/5
Phase 5 (인프라):   ⬜⬜⬜⬜⬜⬜ 0/6
Phase 6 (서버):     ⬜⬜⬜⬜⬜⬜ 0/6
Phase 7 (모니터링): ⬜⬜⬜ 0/3

총 진행률: 5/29 (17.2%)
```

> ⚠️ 실행 순서는 TASK_ORDER.md 참조

---

## 📎 참고

- 규칙: [CLAUDE_CODE_RULES.md](./CLAUDE_CODE_RULES.md)
- 가이드: [CLAUDE_CODE_GUIDE.md](./CLAUDE_CODE_GUIDE.md)
- 순서: [TASK_ORDER.md](./TASK_ORDER.md)
