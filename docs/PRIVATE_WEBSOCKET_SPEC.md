# 거래소별 Private WebSocket 사양서

> **문서 버전**: v1.1
> **최종 수정**: 2026-04-01
> **변경 이력**:
> | 버전 | 날짜 | 내용 |
> |------|------|------|
> | v1.0 | 2026-04-01 | 최초 작성 — 4개 거래소 Private WebSocket 조사 |
> | v1.1 | 2026-04-01 | 바이낸스 WS API 주문 제출 추가, MEXC WS 주문 불가 확인 |

---

## 개요

4개 거래소 모두 Private WebSocket을 지원한다.

| 거래소 | 주문 제출 | 실시간 수신 | 인증 방식 | 데이터 포맷 |
|--------|----------|-----------|-----------|------------|
| 업비트 | REST only | Private WS (`myOrder`) | JWT (HS512) 헤더 | JSON |
| 빗썸 | REST only | Private WS (`myOrder`) | JWT (HS256) 헤더 | JSON |
| 바이낸스 | **REST + WS API 모두 가능** | WS API (`executionReport`) | HMAC 서명 (WS 메시지) | JSON |
| MEXC | REST only | Private WS (listenKey) | listenKey (REST 발급) | **Protobuf** |

> **바이낸스만 유일하게 WebSocket으로 주문 제출이 가능하다.**
> 업비트, 빗썸, MEXC는 주문 제출은 REST, 실시간 업데이트만 WebSocket.

---

## 1. 업비트 (Upbit)

### 엔드포인트

```
wss://api.upbit.com/websocket/v1/private
```

### 인증

WebSocket 핸드셰이크 시 `Authorization` 헤더에 JWT 토큰 포함.

**JWT 페이로드:**
```json
{
  "access_key": "YOUR_ACCESS_KEY",
  "nonce": "UUID_v4",
  "timestamp": 1710146517259
}
```

- 알고리즘: **HS512** (HMAC-SHA512)
- Secret Key는 raw 사용 (Base64 인코딩 아님)
- `nonce`는 매 연결 시 고유해야 함

**헤더:**
```
Authorization: Bearer <jwt_token>
```

### 구독 형식

```json
[
  {"ticket": "unique-uuid"},
  {"type": "myOrder", "codes": ["KRW-XRP"]},
  {"format": "SIMPLE"}
]
```

- `codes` 생략 시 전체 마켓 구독
- `format`: `"DEFAULT"` (전체 필드명) / `"SIMPLE"` (축약 필드명)

### myOrder 응답 필드

| 필드 | SIMPLE | 타입 | 설명 |
|------|--------|------|------|
| type | ty | String | `"myOrder"` |
| code | cd | String | 마켓 코드 (예: KRW-XRP) |
| uuid | uid | String | 주문 고유 ID |
| ask_bid | ab | String | `"ASK"` (매도) / `"BID"` (매수) |
| order_type | ot | String | `"limit"`, `"price"`, `"market"`, `"best"` |
| state | s | String | `"wait"`, `"watch"`, `"trade"`, `"done"`, `"cancel"` |
| trade_uuid | tuid | String | 체결 고유 ID |
| price | p | Double | 주문 가격 |
| avg_price | ap | Double | 평균 체결 가격 |
| volume | v | Double | 주문 수량 |
| remaining_volume | rv | Double | 미체결 수량 |
| executed_volume | ev | Double | 체결 수량 |
| trades_count | tc | Integer | 체결 횟수 |
| paid_fee | pf | Double | 지불 수수료 |
| executed_funds | ef | Double | 체결 금액 |
| trade_fee | tf | Double | 체결 수수료 (state=trade일 때만) |
| is_maker | im | Boolean | 메이커 여부 |
| identifier | id | String | 클라이언트 주문 ID |
| trade_timestamp | ttms | Long | 체결 시각 (ms) |
| order_timestamp | otms | Long | 주문 생성 시각 (ms) |
| timestamp | tms | Long | 메시지 시각 (ms) |
| stream_type | st | String | `"REALTIME"` / `"SNAPSHOT"` |

### myAsset 응답 필드

| 필드 | SIMPLE | 타입 | 설명 |
|------|--------|------|------|
| type | ty | String | `"myAsset"` |
| assets | ast | Array | 자산 배열 |
| assets[].currency | ast.cu | String | 화폐 코드 (예: "KRW") |
| assets[].balance | ast.b | Double | 가용 잔고 |
| assets[].locked | ast.l | Double | 주문 잠금 잔고 |
| asset_timestamp | asttms | Long | 자산 데이터 시각 (ms) |
| timestamp | tms | Long | 메시지 시각 (ms) |

- `myAsset`은 `codes` 필터 미지원 — 전체 자산 전송

### 연결 관리

| 항목 | 값 |
|------|-----|
| 유휴 타임아웃 | 120초 (데이터 없으면 서버 연결 해제) |
| Ping 간격 | 30초 권장 |
| Pong 타임아웃 | 10초 내 미응답 시 연결 해제 |
| 연결 제한 | 초당 5회 |
| 메시지 제한 | 초당 5건, 분당 100건 |
| 재연결 | 최대 3회, 2초 간격 |

### 주의사항

- 주문/체결 이벤트가 없으면 데이터가 오지 않음 (정상)
- `stream_type: "SNAPSHOT"`은 연결 직후 현재 상태, `"REALTIME"`이 실시간 업데이트

---

## 2. 빗썸 (Bithumb)

### 엔드포인트

```
wss://ws-api.bithumb.com/websocket/v1/private
```

### 인증

업비트와 거의 동일한 JWT 방식.

**JWT 페이로드:**
```json
{
  "access_key": "YOUR_API_KEY",
  "nonce": "UUID_v4",
  "timestamp": 1727052318000
}
```

- 알고리즘: **HS256**
- **API 2.0 KEY 필수** (구버전 1.x 키 불가)

**헤더:**
```
Authorization: Bearer <jwt_token>
```

### 구독 형식

```json
[
  {"ticket": "unique-id"},
  {"type": "myOrder", "codes": ["KRW-XRP"]},
  {"format": "SIMPLE"}
]
```

- 업비트와 동일한 구독 포맷

### myOrder 응답 필드

| 필드 | SIMPLE | 타입 | 설명 |
|------|--------|------|------|
| type | ty | String | `"myOrder"` |
| code | cd | String | 마켓 코드 (예: KRW-XRP) |
| client_order_id | coid | String | 클라이언트 주문 ID |
| uuid | uid | String | 주문 고유 ID |
| ask_bid | ab | String | `"ASK"` / `"BID"` |
| order_type | ot | String | `"limit"`, `"price"`, `"market"` |
| state | s | String | `"wait"`, `"trade"`, `"done"`, `"cancel"` |
| trade_uuid | tuid | String | 체결 고유 ID |
| price | p | Double | 주문 가격 |
| volume | v | Double | 주문 수량 |
| remaining_volume | rv | Double | 미체결 수량 |
| executed_volume | ev | Double | 체결 수량 |
| trades_count | tc | Double | 체결 횟수 |
| paid_fee | pf | Double | 지불 수수료 |
| executed_funds | ef | Double | 체결 금액 |
| trade_timestamp | ttms | Long | 체결 시각 (ms) |
| order_timestamp | otms | Long | 주문 생성 시각 (ms) |
| timestamp | tms | Long | 메시지 시각 (ms) |
| stream_type | st | String | `"REALTIME"` |

### myAsset 응답 필드

업비트와 동일 구조 (currency, balance, locked).

### 연결 관리

| 항목 | 값 |
|------|-----|
| 연결 제한 | 초당 10회 (Public + Private 합산) |
| 데이터 수신 | 레이트 리밋 없음 |

### 주의사항

- 업비트와 필드명/구조가 거의 동일 → 공통 추상화 가능
- 구 WebSocket은 2025-10-23 종료됨 — 신규 API만 동작
- 2026-04-02 이후 `canceling_uuid`, `cancel_type` 필드 추가 예정

---

## 3. 바이낸스 (Binance)

### 엔드포인트

```
wss://ws-api.binance.com:443/ws-api/v3
```

> 바이낸스는 **하나의 WS 연결**로 주문 제출 + 실시간 수신을 모두 처리할 수 있다.
> REST API와 rate limit 풀을 공유하지만, TCP/TLS 핸드셰이크를 생략하므로 주문당 ~1-5ms 절감.

### 인증 (2가지 방식)

#### 방식 A: 요청마다 서명 (HMAC-SHA256 — 현재 우리 시스템에 적합)

모든 TRADE/USER_DATA 요청에 `apiKey`, `timestamp`, `signature` 포함.

**서명 생성:**
1. `params`에서 `signature` 제외한 모든 키를 알파벳순 정렬
2. `key=value` 쌍을 `&`로 연결
3. `HMAC-SHA256(payload, secret_key)` → hex 인코딩

#### 방식 B: session.logon (Ed25519 — 추후 마이그레이션 고려)

한 번 인증하면 이후 요청에서 `apiKey`/`signature` 생략 가능.

```json
{
  "id": "uuid",
  "method": "session.logon",
  "params": {
    "apiKey": "YOUR_KEY",
    "signature": "ED25519_SIGNATURE",
    "timestamp": 1649729878532
  }
}
```

- 세션 유지 시간: `session.logout` 또는 연결 종료(최대 24h)까지
- **서명 오버헤드 제거** — 고빈도 주문 시 유리

### WS API 주문 제출

#### order.place (신규 주문)

**요청:**
```json
{
  "id": "56374a46-3061-486b-a311-99ee972eb648",
  "method": "order.place",
  "params": {
    "symbol":      "XRPUSDT",
    "side":        "SELL",
    "type":        "LIMIT",
    "timeInForce": "GTC",
    "price":       "0.6500",
    "quantity":    "100.00",
    "apiKey":      "YOUR_API_KEY",
    "timestamp":   1660801715431,
    "signature":   "HMAC_HEX_STRING"
  }
}
```

**주문 유형별 필수 파라미터:**

| 유형 | 필수 |
|------|------|
| `LIMIT` | `timeInForce`, `price`, `quantity` |
| `LIMIT_MAKER` | `price`, `quantity` |
| `MARKET` | `quantity` 또는 `quoteOrderQty` |

**선택 파라미터:** `newClientOrderId`, `newOrderRespType` (ACK/RESULT/FULL), `recvWindow`

**응답 (FULL):**
```json
{
  "id": "56374a46-3061-486b-a311-99ee972eb648",
  "status": 200,
  "result": {
    "symbol": "XRPUSDT",
    "orderId": 12569099453,
    "clientOrderId": "4d96324ff9d44481926157ec08158a40",
    "transactTime": 1660801715793,
    "price": "0.65000000",
    "origQty": "100.00000000",
    "executedQty": "100.00000000",
    "status": "FILLED",
    "type": "LIMIT",
    "side": "SELL",
    "fills": [
      {
        "price": "0.65000000",
        "qty": "100.00000000",
        "commission": "0.00000000",
        "commissionAsset": "USDT",
        "tradeId": 1650422481
      }
    ]
  },
  "rateLimits": [
    {"rateLimitType": "ORDERS", "interval": "SECOND", "intervalNum": 10, "limit": 50, "count": 1},
    {"rateLimitType": "REQUEST_WEIGHT", "interval": "MINUTE", "intervalNum": 1, "limit": 6000, "count": 1}
  ]
}
```

#### order.cancel (주문 취소)

**요청:**
```json
{
  "id": "uuid",
  "method": "order.cancel",
  "params": {
    "symbol": "XRPUSDT",
    "origClientOrderId": "my_order_123",
    "apiKey": "YOUR_API_KEY",
    "timestamp": 1660801715830,
    "signature": "HMAC_HEX_STRING"
  }
}
```

- `orderId` (Long) 또는 `origClientOrderId` (String) 중 하나 필수
- 선택: `cancelRestrictions` (`ONLY_NEW` | `ONLY_PARTIALLY_FILLED`)

#### order.cancelReplace (원자적 취소 + 신규 주문)

```json
{
  "id": "uuid",
  "method": "order.cancelReplace",
  "params": {
    "symbol": "XRPUSDT",
    "cancelReplaceMode": "STOP_ON_FAILURE",
    "cancelOrigClientOrderId": "old_order",
    "side": "SELL",
    "type": "LIMIT",
    "timeInForce": "GTC",
    "price": "0.6500",
    "quantity": "100.00",
    "apiKey": "YOUR_API_KEY",
    "timestamp": 1660813156900,
    "signature": "HMAC_HEX_STRING"
  }
}
```

- `STOP_ON_FAILURE`: 취소 실패 시 신규 주문도 안 냄
- `ALLOW_FAILURE`: 취소 실패해도 신규 주문 진행

#### order.status (주문 조회)

```json
{
  "id": "uuid",
  "method": "order.status",
  "params": {
    "symbol": "XRPUSDT",
    "orderId": 328999071,
    "apiKey": "YOUR_API_KEY",
    "timestamp": 1703441060152,
    "signature": "HMAC_HEX_STRING"
  }
}
```

#### 전체 WS API 트레이딩 메서드

| 메서드 | 설명 | Weight |
|--------|------|--------|
| `order.place` | 신규 주문 | 1 |
| `order.test` | 테스트 주문 (미실행) | 1 |
| `order.status` | 주문 조회 | 1 |
| `order.cancel` | 주문 취소 | 1 |
| `order.cancelReplace` | 원자적 취소+신규 | 1 |
| `openOrders.status` | 미체결 전체 조회 | 가변 |
| `openOrders.cancelAll` | 미체결 전체 취소 | 1 |

### 실시간 주문 업데이트 구독

같은 WS 연결에서 `userDataStream.subscribe.signature`로 실시간 이벤트 수신:

```json
{
  "id": "uuid",
  "method": "userDataStream.subscribe.signature",
  "params": {
    "apiKey": "YOUR_KEY",
    "signature": "HMAC_SHA256_SIGNATURE",
    "timestamp": 1649729878532
  }
}
```

- HMAC/RSA/Ed25519 모두 지원
- 구독 후 `executionReport`, `outboundAccountPosition` 이벤트 자동 수신

> **레거시 listenKey 방식** (`POST /api/v3/userDataStream` → `wss://stream.binance.com:9443/ws/<listenKey>`)은
> 2025-04 deprecated. 신규 구현은 WS API 방식 권장.

### executionReport 이벤트 (주문 업데이트)

| 필드 | 키 | 타입 | 설명 |
|------|-----|------|------|
| eventType | e | String | `"executionReport"` |
| eventTime | E | Long | 이벤트 시각 (ms) |
| symbol | s | String | `"XRPUSDT"` |
| clientOrderId | c | String | 클라이언트 주문 ID |
| side | S | String | `"BUY"` / `"SELL"` |
| orderType | o | String | `"LIMIT"`, `"MARKET"` |
| timeInForce | f | String | `"GTC"`, `"IOC"`, `"FOK"` |
| orderQty | q | String | 주문 수량 |
| orderPrice | p | String | 주문 가격 |
| executionType | x | String | `"NEW"`, `"TRADE"`, `"CANCELED"`, `"EXPIRED"` |
| orderStatus | X | String | `"NEW"`, `"PARTIALLY_FILLED"`, `"FILLED"`, `"CANCELED"` |
| rejectReason | r | String | `"NONE"` 또는 거부 사유 |
| orderId | i | Long | 바이낸스 주문 ID |
| lastFilledQty | l | String | 최근 체결 수량 |
| cumulativeFilledQty | z | String | 누적 체결 수량 |
| lastFilledPrice | L | String | 최근 체결 가격 |
| commission | n | String | 수수료 |
| commissionAsset | N | String | 수수료 화폐 |
| transactionTime | T | Long | 거래 시각 (ms) |
| tradeId | t | Long | 체결 ID (-1이면 없음) |
| isMaker | m | Bool | 메이커 여부 |
| orderCreationTime | O | Long | 주문 생성 시각 (ms) |
| cumulativeQuoteQty | Z | String | 누적 체결 금액 |

**주의: 모든 가격/수량은 문자열(String)이다.**

### outboundAccountPosition 이벤트 (잔고 변동)

| 필드 | 키 | 타입 | 설명 |
|------|-----|------|------|
| eventType | e | String | `"outboundAccountPosition"` |
| eventTime | E | Long | 이벤트 시각 (ms) |
| balances | B | Array | 잔고 배열 |
| balances[].asset | a | String | 자산 코드 |
| balances[].free | f | String | 가용 잔고 |
| balances[].locked | l | String | 잠금 잔고 |

### 연결 관리

| 항목 | 값 |
|------|-----|
| 서버 Ping | 20초마다 |
| Pong 타임아웃 | 60초 내 미응답 시 연결 해제 |
| listenKey 만료 | 60분 (PUT keepalive 30분마다) |
| 최대 연결 시간 | 24시간 후 자동 해제 |
| 연결 제한 | 5분당 300회 |
| 스트림 제한 | 연결당 1024개 |
| 메시지 제한 | 초당 5건 |

### 주의사항

- listenKey 방식은 deprecated이지만 아직 동작 — 신규 구현은 `userDataStream.subscribe.signature` 권장
- 가격/수량이 모두 String → `std::stod()` 변환 필요

---

## 4. MEXC

> **MEXC는 WebSocket으로 주문 제출이 불가능하다.** WS는 읽기 전용(주문 업데이트 수신만).
> 주문 제출/취소/조회는 REST API만 지원. (선물 WS 주문 API는 2022-07 이후 비활성화 상태)

### listenKey 관리 (REST API)

| 작업 | 메서드 | 경로 |
|------|--------|------|
| 생성 | POST | `/api/v3/userDataStream` |
| 갱신 | PUT | `/api/v3/userDataStream` |
| 종료 | DELETE | `/api/v3/userDataStream` |

- 권한: `SPOT_ACCOUNT_READ`
- listenKey 만료: 60분 (PUT keepalive 30분마다)

### 엔드포인트

```
wss://wbs.mexc.com/ws?listenKey=<YOUR_LISTEN_KEY>
```

### 구독

```json
{
  "method": "SUBSCRIPTION",
  "params": ["spot@private.orders.v3.api"],
  "id": 1
}
```

### ⚠️ 데이터 포맷: Protobuf

**MEXC Private 채널은 JSON이 아닌 Protocol Buffers로 인코딩된다.**

Proto 정의: https://github.com/mexcdevelop/websocket-proto

### 주문 채널: `spot@private.orders.v3.api`

Proto 메시지 `PrivateOrdersV3Api`:

| 필드 | 타입 | 번호 | 설명 |
|------|------|------|------|
| id | string | 1 | 주문 ID |
| clientId | string | 2 | 클라이언트 주문 ID |
| price | string | 3 | 가격 |
| quantity | string | 4 | 수량 |
| amount | string | 5 | 금액 |
| avgPrice | string | 6 | 평균 체결 가격 |
| orderType | int32 | 7 | 주문 유형 |
| tradeType | int32 | 8 | 매매 방향 (BUY/SELL) |
| isMaker | bool | 9 | 메이커 여부 |
| remainAmount | string | 10 | 잔여 금액 |
| remainQuantity | string | 11 | 잔여 수량 |
| lastDealQuantity | string | 12 | 최근 체결 수량 (optional) |
| cumulativeQuantity | string | 13 | 누적 체결 수량 |
| cumulativeAmount | string | 14 | 누적 체결 금액 |
| status | int32 | 15 | 주문 상태 |
| createTime | int64 | 16 | 생성 시각 |

### 체결 채널: `spot@private.deals.v3.api`

Proto 메시지 `PrivateDealsV3Api`:

| 필드 | 타입 | 번호 | 설명 |
|------|------|------|------|
| price | string | 1 | 체결 가격 |
| quantity | string | 2 | 체결 수량 |
| amount | string | 3 | 체결 금액 |
| tradeType | int32 | 4 | 매매 방향 |
| isMaker | bool | 5 | 메이커 여부 |
| tradeId | string | 7 | 체결 ID |
| orderId | string | 9 | 주문 ID |
| feeAmount | string | 10 | 수수료 |
| feeCurrency | string | 11 | 수수료 화폐 |
| time | int64 | 12 | 체결 시각 |

### 잔고 채널: `spot@private.account.v3.api`

Proto 메시지 `PrivateAccountV3Api`:

| 필드 | 타입 | 번호 | 설명 |
|------|------|------|------|
| vcoinName | string | 1 | 코인명 |
| balanceAmount | string | 3 | 잔고 |
| balanceAmountChange | string | 4 | 잔고 변동 |
| frozenAmount | string | 5 | 동결 잔고 |
| frozenAmountChange | string | 6 | 동결 변동 |

### 연결 관리

| 항목 | 값 |
|------|-----|
| 구독 없이 연결 | 30초 후 해제 |
| 데이터 없이 연결 | 60초 후 해제 |
| listenKey 만료 | 60분 (PUT keepalive 30분마다) |
| 최대 연결 시간 | 24시간 |
| listenKey 제한 | UID당 최대 60개 |
| 연결 제한 | listenKey당 5개, UID당 300개 |
| 채널 제한 | 연결당 30개 |

### 주의사항

- **Protobuf 디시리얼라이제이션 필수** — JSON이 아님
- `.proto` 파일 컴파일 후 C++ 코드 생성 필요 (`protoc`)
- `orderType`, `tradeType`, `status`는 정수 enum
- CMake에 `libprotobuf` 링크 추가 필요

---

## 구현 전략

### 공통 추상화 가능 영역

**업비트 + 빗썸:**
- JWT 인증 (알고리즘만 다름: HS512 vs HS256)
- 동일한 구독 형식 (`ticket` + `type` + `codes`)
- 거의 동일한 `myOrder` 필드
- → **하나의 베이스 클래스로 통합 가능**

### 거래소별 차이점

```
           주문 제출    실시간 수신     인증          데이터 포맷    파싱
업비트     REST         Private WS     JWT 헤더      JSON          simdjson
빗썸       REST         Private WS     JWT 헤더      JSON          simdjson
바이낸스   WS API       WS API         WS 메시지     JSON          simdjson
MEXC       REST         Private WS     listenKey     Protobuf      protoc 생성 코드
```

> **바이낸스는 하나의 WS 연결로 주문 제출 + 실시간 수신을 모두 처리.**
> 나머지 3개 거래소는 REST(주문) + Private WS(수신) 이원 구조.

### client_order_id 기반 주문 추적

주문 추적의 핵심은 `client_order_id`이다.
거래소 발급 `order_id`는 REST 응답이 와야 알 수 있지만,
`client_order_id`는 **주문 제출 전에 우리가 생성**하므로 응답을 기다리지 않고 즉시 추적 가능하다.

```
submit() 시점:
  client_order_id 생성 ("arb_{request_id}_{buy|sell}")
       ↓
  OrderTracker에 등록 (매수/매도 쌍을 request_id로 묶음)
       ↓
  거래소에 전송 (client_order_id 포함) → 즉시 리턴 (fire-and-forget)

Private WS 수신 시:
  거래소 → OrderUpdate (client_order_id 포함) → OrderTracker가 매칭
```

**거래소별 client_order_id 필드명:**

| 거래소 | Private WS 필드 | 주문 제출 시 파라미터 |
|--------|----------------|---------------------|
| 업비트 | `identifier` (SIMPLE: `id`) | `identifier` |
| 빗썸 | `client_order_id` (SIMPLE: `coid`) | `client_order_id` |
| 바이낸스 | `clientOrderId` (키: `c`) | `newClientOrderId` |
| MEXC | `clientId` (Protobuf 필드 2) | `newClientOrderId` |

### 수신 데이터 → 내부 구조체 매핑

모든 거래소의 Private 메시지를 아래 공통 구조체로 변환:

```
거래소 Private WS 메시지
    ↓ (거래소별 파서)
OrderUpdate {
    exchange: Exchange
    order_id: string              ← 거래소 발급 ID
    client_order_id: string       ← ★ 핵심 매칭 키 (우리가 생성)
    status: OrderStatus (Open/PartiallyFilled/Filled/Canceled/Failed)
    side: OrderSide
    filled_qty: double
    remaining_qty: double
    avg_price: double
    last_fill_price: double
    last_fill_qty: double
    commission: double
    timestamp_ms: int64
    is_maker: bool
}
    ↓ (SPSC Queue)
OrderTracker (client_order_id로 매수/매도 쌍 집계)
    ↓ (completion callback)
RecoveryManager / Stats
```

---

## 참고 문서

| 거래소 | URL |
|--------|-----|
| 업비트 Private WS | https://docs.upbit.com/kr/reference/websocket-myorder |
| 업비트 인증 | https://docs.upbit.com/kr/reference/auth |
| 빗썸 Private WS | https://apidocs.bithumb.com (v2.1.5) |
| 바이낸스 User Data Stream | https://developers.binance.com/docs/binance-spot-api-docs/user-data-stream |
| 바이낸스 WS API 주문 | https://developers.binance.com/docs/binance-spot-api-docs/websocket-api/trading-requests |
| 바이낸스 WS API 인증 | https://developers.binance.com/docs/binance-spot-api-docs/websocket-api/authentication-requests |
| MEXC User Data Stream | https://mexcdevelop.github.io/apidocs/spot_v3_en/#websocket-user-data-streams |
| MEXC Protobuf 정의 | https://github.com/mexcdevelop/websocket-proto |
