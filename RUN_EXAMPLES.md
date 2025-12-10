# 예제 프로그램 실행 가이드

## 빌드 확인
```bash
# 빌드가 안 되어 있으면 먼저 빌드
mkdir -p build && cd build
cmake ..
cmake --build . -j 8
cd ..
```

## 예제 프로그램 목록

### 1. WebSocket 실시간 가격 수신
```bash
./build/bin/websocket_example
```
- 업비트, 바이낸스, 빗썸, MEXC에서 XRP, BTC, ETH 실시간 가격 수신
- Ctrl+C로 종료

### 2. 김치 프리미엄 계산
```bash
./build/bin/premium_example
```
- 실제 환율 데이터 가져오기 (exchangerate-api.com)
- 4개 거래소 간 프리미엄 매트릭스 표시
- XRP 기준 실시간 프리미엄 모니터링 (30초간)

### 3. 환율 서비스
```bash
./build/bin/fxrate_example
```
- USD/KRW 실시간 환율 조회
- 자동 갱신 및 캐싱 기능
- 김치 프리미엄 계산 예시

### 4. 주문 API 테스트
```bash
./build/bin/order_example
```
- 주문 생성 및 서명 테스트
- API 키가 없어도 동작 (DRY RUN 모드)
- 거래소별 주문 형식 확인

## 설정 파일

`config/config.yaml` 파일에서 모니터링할 코인을 변경할 수 있습니다:

```yaml
symbols:
  primary:
    - symbol: "XRP"
      upbit: "KRW-XRP"
      bithumb: "XRP_KRW"
      binance: "XRPUSDT"
      mexc: "XRPUSDT"
      enabled: true
```

## 실행 시 출력 예시

### WebSocket 예제
```
Loaded symbols from config:
  Upbit: KRW-XRP KRW-BTC KRW-ETH 
  Binance: XRPUSDT BTCUSDT ETHUSDT 
  ...

[upbit] Ticker: XRP @ 3096
[binance] Ticker: XRPUSDT @ 2.0883
```

### Premium 예제
```
=== Premium Calculator Example (with Real Data) ===

Fetching real FX rate...
FX Rate: 1468.93 KRW/USD

=== Premium Matrix (%) ===

       Buy →
Sell ↓     Upbit  Bithumb  Binance     MEXC
  Upbit     0.00      N/A     0.94      N/A
  ...
```

## 주의사항

1. 인터넷 연결이 필요합니다 (실제 API 사용)
2. 빗썸과 MEXC는 간헐적으로 연결이 불안정할 수 있습니다
3. API 키 없이도 가격 조회는 가능하지만, 주문은 불가능합니다