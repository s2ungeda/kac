# Binance 피드 수정 + ASan/ASLR 크래시 분석 (2026-06-11)

> TASK_38 (Feeder Executables) 검증 중 발견된 이슈 2건의 원인 분석과 수정 기록

---

## 이슈 1: Binance 피더 체결/호가 데이터 미수신

### 증상

`binance-feeder` 실행 시 WebSocket 연결은 성공하지만:

- `ticks=0/0` — 체결(trade) 이벤트 0건
- `ob=77/77` — 호가 이벤트는 수신되지만 **호가 레벨이 전부 비어 있음** (bid_count=0, best_bid=0)
- 빈 OrderBook이 SHM에 그대로 push됨 → 엔진이 소비하면 김프 계산이 0 가격으로 오염될 수 있는 상태

### 원인 (2건 복합)

**1) `@aggTrade` 스트림이 fstream에서 데이터를 보내지 않음**

Python 원시 WebSocket 클라이언트로 교차 검증한 결과 (10초 수신 기준):

| 스트림 | 메시지 수 |
|--------|----------|
| `wss://fstream.binance.com/stream?streams=xrpusdt@aggTrade` | **0건** |
| `wss://fstream.binance.com/stream?streams=xrpusdt@trade` | 48건 |
| `wss://fstream.binance.com/stream?streams=xrpusdt@depth10` | 38건 |
| `wss://stream.binance.com:9443/stream?streams=xrpusdt@trade` (스팟) | 11건 |

같은 연결 방식에서 `@trade`는 정상 수신되므로 클라이언트 코드 문제가 아니라
서버 측에서 `@aggTrade` 스트림이 무응답인 현상 (BTCUSDT로도 동일 재현).
REST `fapi/v1/aggTrades`는 정상 응답하므로 네트워크 차단도 아님.

**2) 선물 depth 페이로드의 호가 배열 키 불일치**

| 엔드포인트 | depth 페이로드 호가 키 |
|-----------|----------------------|
| 스팟 (`stream.binance.com`) partial depth | `"bids"` / `"asks"` |
| 선물 (`fstream.binance.com`) depthUpdate | `"b"` / `"a"` |

`BINANCE_OB_MAP`이 스팟 형식(`"bids"/"asks"`)만 찾고 있어서, 선물 메시지에서는
키 조회가 조용히 실패 → 레벨 0개인 OrderBook이 이벤트로 방출되고 있었음.
(`make_orderbook`은 키가 없어도 에러 없이 빈 호가를 반환)

### 수정

| 파일 | 변경 |
|------|------|
| `src/feeder/feeder_process.cpp` | Binance target 빌드 시 `@aggTrade` → `@trade` |
| `src/app/application.cpp` | 엔진 Standalone 모드 연결 URL `@aggTrade` → `@trade` |
| `src/exchange/binance/websocket.cpp` | OB 맵을 스팟/선물 2종으로 분리, 페이로드에 `"b"` 키 존재 여부로 자동 선택 |

수신 측 파싱 분기(`@aggTrade`, `"e":"aggTrade"`)는 호환성을 위해 유지.

### 수정 후 검증 (Release 빌드, 실연결 20초)

```
binance feeder: ticks=305/305 ob=77/77 dropped=0/0 reconnects=0 (20s)
[Binance] OrderBook - Symbol: XRPUSDT, BestBid: 1.1147, BestAsk: 1.1148
[Binance] Trade - Symbol: XRPUSDT, Price: 1.1148 USDT
```

4개 피더 전체 수신 현황 (20초):

| 피더 | ticks | ob |
|------|-------|-----|
| upbit | 11 | 53 |
| bithumb | 6 | 97 |
| binance | 305 | 77 |
| mexc | 4 | 67 |

---

## 이슈 2: Debug(ASan) 빌드 간헐적 시작 크래시

### 증상

Debug 빌드 실행 파일이 **약 40% 확률로 시작 즉시 크래시** (main() 진입 전):

- 로그 한 줄 없이 `AddressSanitizer:DEADLYSIGNAL`이 무한 반복 출력 (수백 MB)
- dmesg: `segfault ... in ld-2.31.so` (동적 로더 내부)
- 특정 바이너리 문제가 아님 — 같은 바이너리가 됐다 안 됐다 함

### 원인

**GCC 9의 구버전 ASan ↔ 커널 6.5+ 고엔트로피 ASLR 비호환** (알려진 문제).

- 커널 6.5부터 `vm.mmap_rnd_bits` 기본값이 32로 상향 (현재 WSL2 커널 6.18)
- GCC 13.2 미만의 ASan은 shadow memory 매핑이 고엔트로피 주소와 충돌하면
  로더 단계에서 SEGV → ASan 시그널 핸들러도 같이 죽어 무한 루프
- ASLR 주소가 매 실행마다 랜덤이라 간헐적으로 발생

### 검증 (upbit-feeder 12회 반복 실행)

| 조건 | 성공 | 크래시 |
|------|------|--------|
| ASLR on (기본) | 7 | **5** |
| ASLR off (`setarch -R`) | **12** | 0 |

### 대응

1. **임시 (현재 적용)**: Debug 바이너리/테스트 실행 시 `setarch $(uname -m) -R` 래핑
   ```bash
   setarch $(uname -m) -R ./build/bin/upbit-feeder
   setarch $(uname -m) -R ctest --test-dir build
   ```
2. **영구 (sudo 필요, 사용자 직접 실행)**:
   ```bash
   sudo sysctl vm.mmap_rnd_bits=28
   # 영구 적용:
   echo 'vm.mmap_rnd_bits=28' | sudo tee /etc/sysctl.d/99-asan.conf
   ```
3. **근본**: GCC 13.2+ 또는 Clang 18+로 업그레이드 시 해결됨
4. Release 빌드(`build-release/`)는 ASan이 없으므로 영향 없음 — 운영 실행은 Release 사용

> 참고: 이 문제는 Debug 빌드의 **모든** 실행 파일/테스트에 영향. 테스트가 이유 없이
> 무한 대기하거나 DEADLYSIGNAL을 뿜으면 이 문제를 먼저 의심할 것.
