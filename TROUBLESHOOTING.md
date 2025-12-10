# 문제 해결 가이드

## WebSocket 연결 문제

### MEXC "Connection reset by peer" 오류

MEXC WebSocket이 간헐적으로 연결이 끊기는 문제가 발생할 수 있습니다.

**원인:**
1. MEXC 서버 측의 연결 제한
2. 구독 메시지 형식 문제
3. Keep-alive ping/pong 메커니즘 부재

**해결 방법:**

1. **Ping/Pong 메시지 추가** (권장)
   - MEXC는 주기적인 ping 메시지를 요구할 수 있습니다
   - 30초마다 ping 메시지 전송

2. **재연결 로직 확인**
   - 현재 자동 재연결이 구현되어 있으므로 일시적 끊김은 자동 복구됩니다
   - 로그에서 "Reconnecting" 메시지 확인

3. **구독 수 제한**
   - 한 번에 너무 많은 심볼을 구독하면 연결이 끊길 수 있습니다
   - config.yaml에서 enabled: false로 일부 심볼 비활성화

### Bithumb WebSocket 연결 문제

Bithumb도 유사한 연결 문제가 발생할 수 있습니다.

**해결 방법:**
- Bithumb은 특수한 구독 형식을 사용합니다
- 현재 자동 재연결로 대부분 해결됩니다

## 예제 실행 시 팁

### 1. 특정 거래소만 테스트
```bash
# 환경 변수로 특정 거래소만 활성화 (향후 구현 예정)
ENABLE_EXCHANGES=upbit,binance ./build/bin/websocket_example
```

### 2. 로그 레벨 조정
```bash
# 더 자세한 로그 보기
LOG_LEVEL=debug ./build/bin/websocket_example
```

### 3. 안정적인 거래소 우선 사용
- **가장 안정적**: Upbit, Binance
- **간헐적 문제**: Bithumb, MEXC

## 일반적인 오류 메시지

### "invalid_access_key"
- 주문 API 사용 시 API 키가 없거나 잘못됨
- 가격 조회는 API 키 없이도 가능

### "Mandatory parameter 'signature' was not sent"
- Binance API 서명 오류
- API Secret이 설정되지 않음

### "yaml-cpp not found, using default configuration"
- yaml-cpp 라이브러리가 설치되지 않음
- 기본 설정(XRP, BTC, ETH)으로 동작

## 성능 최적화 팁

1. **CPU 사용률이 높은 경우**
   - Busy polling 방식이므로 정상입니다
   - 실제 프로덕션에서는 CPU 코어를 전용으로 할당

2. **메모리 사용량 증가**
   - OrderBook 데이터 누적일 수 있음
   - 주기적으로 오래된 데이터 정리 필요

## 문의사항

추가 문제가 발생하면 다음 정보와 함께 문의하세요:
- 실행한 명령어
- 오류 메시지 전체
- 운영체제 및 컴파일러 버전