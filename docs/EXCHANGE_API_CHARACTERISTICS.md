# 거래소 API 특성 (C++ 구현 참고)

## 업비트 (Upbit)

### WebSocket
- URL: `wss://api.upbit.com/websocket/v1`
- 인증: 불필요 (공개 스트림)
- 구독 형식: JSON 배열
- PING: 서버에서 전송, 응답 필수

### REST API
- 인증: JWT (HS256)
- query_hash: SHA512
- Rate Limit: 초당 8회, 분당 600회

---

## 바이낸스 (Binance)

### WebSocket
- URL: `wss://stream.binance.com:9443/stream?streams=...`
- Combined Stream 사용
- 24시간마다 재연결 필수
- PING: WebSocket 프레임

### REST API
- 인증: HMAC-SHA256
- timestamp 필수 (서버 시간 동기화)
- Rate Limit: 분당 1200회

---

## 빗썸 (Bithumb)

### WebSocket
- URL: `wss://pubwss.bithumb.com/pub/ws`
- 심볼 형식: `XRP_KRW`
- 독특한 구독 형식

### REST API
- 인증: HMAC-SHA512
- Content-Type: application/x-www-form-urlencoded
- Rate Limit: 초당 15회

---

## MEXC

### WebSocket
- URL: `wss://wbs.mexc.com/ws`
- JSON-RPC 스타일
- 30초마다 PING 필수

### REST API
- 인증: HMAC-SHA256 (바이낸스 유사)
- Rate Limit: 초당 10회

---

## C++ 구현 시 공통 주의사항

1. **libcurl 멀티스레드**: `curl_global_init()` 한 번만
2. **OpenSSL 스레드 안전**: 락 콜백 설정 필요 (OpenSSL 1.1+ 불필요)
3. **타임스탬프**: 밀리초 단위, 서버 시간 동기화
4. **재연결**: 모든 WebSocket에 자동 재연결 구현
