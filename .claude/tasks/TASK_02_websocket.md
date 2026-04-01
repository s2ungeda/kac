# TASK 02: WebSocket 클라이언트 (4개 거래소)

## 🎯 목표
Boost.Beast + Boost.Asio 기반 4개 거래소 실시간 시세 수신

---

## ⚠️ 주의사항

```
절대 금지:
- 블로킹 I/O
- 동기 SSL
- std::mutex 직접 사용 (Lock-Free Queue 사용)
- 콜백에서 무거운 작업

필수:
- Boost.Beast + Boost.Asio 비동기 패턴
- 자동 재연결 (지수 백오프)
- PING/PONG 처리
- SSL/TLS 필수 (wss://)
- Lock-Free Queue로 메인 스레드에 데이터 전달
```

---

## 📁 생성할 파일

```
include/arbitrage/exchange/
├── websocket_base.hpp          # 공통 베이스 클래스
├── upbit/
│   ├── websocket.hpp
│   └── types.hpp
├── binance/
│   ├── websocket.hpp
│   └── types.hpp
├── bithumb/
│   ├── websocket.hpp
│   └── types.hpp
└── mexc/
    ├── websocket.hpp
    └── types.hpp

src/exchange/
├── websocket_base.cpp
├── upbit/websocket.cpp
├── binance/websocket.cpp
├── bithumb/websocket.cpp
└── mexc/websocket.cpp

tests/unit/exchange/
└── websocket_test.cpp          # 4개 거래소 통합 테스트
```

---

## 📊 거래소별 차이점

| 항목 | 업비트 | 바이낸스 | 빗썸 | MEXC |
|------|--------|----------|------|------|
| **URL** | wss://api.upbit.com/websocket/v1 | wss://stream.binance.com:9443/ws | wss://pubwss.bithumb.com/pub/ws | wss://wbs.mexc.com/ws |
| **인증** | 없음 | 없음 | 없음 | 없음 |
| **구독 형식** | JSON Array | stream 파라미터 | JSON | JSON |
| **심볼 형식** | KRW-XRP | xrpusdt | XRP_KRW | XRPUSDT |
| **PING 간격** | 30초 | 자동 (내장) | 30초 | 30초 |
| **메시지 형식** | JSON | JSON | JSON | JSON |

---

## 📝 공통 베이스 클래스

### websocket_base.hpp

```cpp
#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <string>
#include <atomic>
#include <functional>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace arbitrage {

// 공통 WebSocket 이벤트
struct WebSocketEvent {
    enum class Type { Ticker, OrderBook, Trade, Connected, Disconnected, Error };
    Type type;
    Exchange exchange;
    Ticker ticker;
    OrderBook orderbook;
    std::string error_message;
};

// 공통 WebSocket 베이스 클래스
class WebSocketClientBase : public std::enable_shared_from_this<WebSocketClientBase> {
public:
    WebSocketClientBase(net::io_context& ioc, ssl::context& ctx, Exchange exchange);
    virtual ~WebSocketClientBase();
    
    // 공통 인터페이스
    void connect(const std::string& host, const std::string& port, const std::string& target);
    void disconnect();
    bool is_connected() const { return connected_.load(); }
    
    // 이벤트 콜백
    using EventCallback = std::function<void(const WebSocketEvent&)>;
    void on_event(EventCallback cb) { event_callback_ = std::move(cb); }
    
    // 이벤트 큐 (Lock-Free, 메인 스레드에서 폴링)
    LockFreeQueue<WebSocketEvent>& event_queue() { return event_queue_; }
    
    // 통계
    struct Stats {
        uint64_t messages_received{0};
        uint64_t bytes_received{0};
        uint64_t reconnect_count{0};
        std::chrono::steady_clock::time_point connected_at;
    };
    Stats get_stats() const { return stats_; }
    
protected:
    // 거래소별 구현 필요
    virtual std::string build_subscribe_message() = 0;
    virtual void parse_message(const std::string& message) = 0;
    virtual std::chrono::seconds ping_interval() { return std::chrono::seconds(30); }
    
    // 이벤트 발행 (파생 클래스에서 호출)
    void emit_event(WebSocketEvent&& evt);
    
    // Boost.Beast 핸들러 (공통)
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_close(beast::error_code ec);
    void on_ping_timer(beast::error_code ec);
    
    void do_read();
    void do_write(const std::string& message);
    void do_ping();
    void schedule_reconnect();
    void fail(beast::error_code ec, char const* what);
    
protected:
    Exchange exchange_;
    net::io_context& ioc_;
    ssl::context& ctx_;
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    net::steady_timer ping_timer_;
    net::steady_timer reconnect_timer_;
    
    std::string host_, port_, target_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> should_reconnect_{true};
    int reconnect_count_{0};
    
    EventCallback event_callback_;
    LockFreeQueue<WebSocketEvent> event_queue_{4096};
    std::vector<std::string> write_queue_;
    bool writing_{false};
    
    Stats stats_;
    std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace arbitrage
```

---

## 📝 거래소별 구현

### 1. 업비트 (Upbit)

```cpp
// upbit/websocket.hpp
class UpbitWebSocket : public WebSocketClientBase {
public:
    UpbitWebSocket(net::io_context& ioc, ssl::context& ctx)
        : WebSocketClientBase(ioc, ctx, Exchange::Upbit) {}
    
    void subscribe_ticker(const std::vector<std::string>& symbols);
    void subscribe_orderbook(const std::vector<std::string>& symbols);
    
protected:
    std::string build_subscribe_message() override;
    void parse_message(const std::string& message) override;
    
private:
    std::vector<std::string> ticker_symbols_;
    std::vector<std::string> orderbook_symbols_;
};

// 구독 메시지 형식
// [{"ticket":"unique-id"},{"type":"ticker","codes":["KRW-XRP"],"isOnlyRealtime":true}]
```

### 2. 바이낸스 (Binance)

```cpp
// binance/websocket.hpp
class BinanceWebSocket : public WebSocketClientBase {
public:
    BinanceWebSocket(net::io_context& ioc, ssl::context& ctx)
        : WebSocketClientBase(ioc, ctx, Exchange::Binance) {}
    
    void subscribe_ticker(const std::vector<std::string>& symbols);
    void subscribe_orderbook(const std::vector<std::string>& symbols, int depth = 10);
    
protected:
    std::string build_subscribe_message() override;
    void parse_message(const std::string& message) override;
    std::chrono::seconds ping_interval() override { return std::chrono::seconds(0); } // 자동
    
private:
    std::vector<std::string> streams_;
};

// 연결 URL 형식: wss://stream.binance.com:9443/stream?streams=xrpusdt@ticker/xrpusdt@depth10
// 또는 구독 메시지: {"method":"SUBSCRIBE","params":["xrpusdt@ticker"],"id":1}
```

### 3. 빗썸 (Bithumb)

```cpp
// bithumb/websocket.hpp
class BithumbWebSocket : public WebSocketClientBase {
public:
    BithumbWebSocket(net::io_context& ioc, ssl::context& ctx)
        : WebSocketClientBase(ioc, ctx, Exchange::Bithumb) {}
    
    void subscribe_ticker(const std::vector<std::string>& symbols);
    void subscribe_orderbook(const std::vector<std::string>& symbols);
    
protected:
    std::string build_subscribe_message() override;
    void parse_message(const std::string& message) override;
    
private:
    std::vector<std::string> symbols_;
};

// 구독 메시지 형식
// {"type":"ticker","symbols":["XRP_KRW"],"tickTypes":["24H"]}
// {"type":"orderbookdepth","symbols":["XRP_KRW"]}
```

### 4. MEXC

```cpp
// mexc/websocket.hpp
class MEXCWebSocket : public WebSocketClientBase {
public:
    MEXCWebSocket(net::io_context& ioc, ssl::context& ctx)
        : WebSocketClientBase(ioc, ctx, Exchange::MEXC) {}
    
    void subscribe_ticker(const std::vector<std::string>& symbols);
    void subscribe_orderbook(const std::vector<std::string>& symbols);
    
protected:
    std::string build_subscribe_message() override;
    void parse_message(const std::string& message) override;
    
private:
    std::vector<std::string> symbols_;
};

// 구독 메시지 형식
// {"method":"SUBSCRIPTION","params":["spot@public.miniTicker.v3.api@XRPUSDT"]}
```

---

## 🔧 사용 예시

```cpp
#include "arbitrage/exchange/upbit/websocket.hpp"
#include "arbitrage/exchange/binance/websocket.hpp"
#include "arbitrage/exchange/bithumb/websocket.hpp"
#include "arbitrage/exchange/mexc/websocket.hpp"

int main() {
    net::io_context ioc;
    ssl::context ctx{ssl::context::tlsv12_client};
    ctx.set_default_verify_paths();
    
    // 4개 거래소 WebSocket 생성
    auto upbit = std::make_shared<UpbitWebSocket>(ioc, ctx);
    auto binance = std::make_shared<BinanceWebSocket>(ioc, ctx);
    auto bithumb = std::make_shared<BithumbWebSocket>(ioc, ctx);
    auto mexc = std::make_shared<MEXCWebSocket>(ioc, ctx);
    
    // 공통 이벤트 핸들러
    auto handler = [](const WebSocketEvent& evt) {
        if (evt.type == WebSocketEvent::Type::Ticker) {
            std::cout << exchange_name(evt.exchange) << ": " 
                      << evt.ticker.price << "\n";
        }
    };
    
    upbit->on_event(handler);
    binance->on_event(handler);
    bithumb->on_event(handler);
    mexc->on_event(handler);
    
    // 구독 설정
    upbit->subscribe_ticker({"KRW-XRP"});
    binance->subscribe_ticker({"xrpusdt"});
    bithumb->subscribe_ticker({"XRP_KRW"});
    mexc->subscribe_ticker({"XRPUSDT"});
    
    // 연결
    upbit->connect("api.upbit.com", "443", "/websocket/v1");
    binance->connect("stream.binance.com", "9443", "/ws");
    bithumb->connect("pubwss.bithumb.com", "443", "/pub/ws");
    mexc->connect("wbs.mexc.com", "443", "/ws");
    
    // I/O 스레드 실행
    ioc.run();
}
```

---

## ⚠️ 거래소별 통신 방식 정리

```
┌─────────────────────────────────────────────────────────────────┐
│  거래소별 WebSocket 채널 구분                                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  [Public WS]  시세/호가/체결 수신 (인증 불필요)                  │
│               → 이 태스크 전반부 (기 구현 완료)                  │
│                                                                 │
│  [Private WS] 주문 체결/취소/잔고 실시간 수신 (인증 필요)        │
│               → 이 태스크 후반부 (미구현 — 아래 참조)            │
│                                                                 │
│  [주문 제출]  REST API 또는 WS API (TASK_03)                    │
│               → 바이낸스만 WS API 주문 가능                      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 📡 Private WebSocket — 실시간 주문 업데이트 수신

> **참고 사양서**: `docs/PRIVATE_WEBSOCKET_SPEC.md`

### 거래소별 Private WS 요약

| 거래소 | 엔드포인트 | 인증 | 포맷 | 주문 채널 | 잔고 채널 |
|--------|-----------|------|------|-----------|-----------|
| 업비트 | `wss://api.upbit.com/websocket/v1/private` | JWT (HS512) 헤더 | JSON | `myOrder` | `myAsset` |
| 빗썸 | `wss://ws-api.bithumb.com/websocket/v1/private` | JWT (HS256) 헤더 | JSON | `myOrder` | `myAsset` |
| 바이낸스 | `wss://ws-api.binance.com:443/ws-api/v3` | HMAC 서명 (WS 메시지) | JSON | `executionReport` | `outboundAccountPosition` |
| MEXC | `wss://wbs.mexc.com/ws?listenKey=<KEY>` | listenKey (REST 발급) | **Protobuf** | `spot@private.orders.v3.api` | `spot@private.account.v3.api` |

### 공통 출력 구조체

모든 거래소의 Private 메시지를 아래 공통 구조체로 변환:

```cpp
struct OrderUpdate {
    Exchange exchange;
    char order_id[48];          // 거래소 발급 ID (REST 응답 대기 없이도)
    char client_order_id[48];   // ★ 핵심 매칭 키 — 제출 전 우리가 생성한 ID
                                // OrderTracker가 이 ID로 주문 쌍(매수/매도)을 추적
                                // 형식: "arb_{request_id}_{buy|sell}"
    OrderStatus status;         // Open/PartiallyFilled/Filled/Canceled/Failed
    OrderSide side;
    double filled_qty;
    double remaining_qty;
    double avg_price;
    double last_fill_price;
    double last_fill_qty;
    double commission;
    int64_t timestamp_ms;
    bool is_maker;
};
```

### 1. 업비트 Private WS

```cpp
// 인증: WebSocket 핸드셰이크 시 Authorization 헤더에 JWT 포함
// JWT: HS512, payload = {access_key, nonce(UUID), timestamp}

// 구독 메시지
[
  {"ticket": "unique-uuid"},
  {"type": "myOrder", "codes": ["KRW-XRP"]},
  {"format": "SIMPLE"}
]

// 수신 필드 (SIMPLE 모드)
// ty="myOrder", cd, uid, ab(ASK/BID), s(wait/trade/done/cancel),
// ev(체결수량), rv(미체결수량), ap(평균가), pf(수수료), ttms, otms
// ★ id(identifier) → client_order_id로 매핑
```

- Ping 30초, 유휴 120초 타임아웃
- 연결 제한: 초당 5회, 메시지 초당 5건

### 2. 빗썸 Private WS

```cpp
// 인증: 업비트와 동일 방식 (JWT 헤더, HS256)
// API 2.0 KEY 필수

// 구독 메시지 (업비트와 동일 형식)
[
  {"ticket": "unique-id"},
  {"type": "myOrder", "codes": ["KRW-XRP"]},
  {"format": "SIMPLE"}
]

// 수신 필드: 업비트와 거의 동일
// ★ coid(client_order_id) → client_order_id로 매핑
```

- 업비트와 공통 베이스 클래스로 통합 가능

### 3. 바이낸스 Private WS

```cpp
// 인증: 같은 WS 연결에서 메시지로 인증
{
  "id": "uuid",
  "method": "userDataStream.subscribe.signature",
  "params": {
    "apiKey": "YOUR_KEY",
    "signature": "HMAC_SHA256_SIGNATURE",
    "timestamp": 1649729878532
  }
}

// 수신 이벤트: executionReport
// 주요 필드: e, s, S(side), o(type), x(executionType), X(orderStatus),
// i(orderId), l(lastFilledQty), z(cumulativeFilledQty), L(lastFilledPrice),
// n(commission), m(isMaker)
// ★ c(clientOrderId) → client_order_id로 매핑
// ※ 모든 가격/수량이 String
```

- 같은 연결로 주문 제출도 가능 (TASK_03 참조)
- Pong 60초 내 응답 필요, 최대 24시간

### 4. MEXC Private WS

```cpp
// 인증: REST로 listenKey 발급 후 URL 파라미터로 전달
// POST /api/v3/userDataStream → {"listenKey": "..."}
// 연결: wss://wbs.mexc.com/ws?listenKey=<KEY>

// 구독 메시지
{"method": "SUBSCRIPTION", "params": ["spot@private.orders.v3.api"], "id": 1}

// ⚠️ Protobuf 인코딩 — JSON이 아님
// proto: PrivateOrdersV3Api (id, clientId, price, quantity, avgPrice, status, ...)
// ★ clientId → client_order_id로 매핑
// protoc로 C++ 코드 생성 필요, CMake에 libprotobuf 추가
```

- listenKey 60분 만료 (PUT keepalive 30분마다)
- 구독 없이 30초, 데이터 없이 60초 후 연결 해제

### 📁 추가 생성할 파일

```
include/arbitrage/exchange/
├── private_ws_base.hpp              # Private WS 공통 베이스
├── upbit/private_websocket.hpp      # 업비트 Private WS
├── binance/private_websocket.hpp    # 바이낸스 Private WS
├── bithumb/private_websocket.hpp    # 빗썸 Private WS
└── mexc/private_websocket.hpp       # MEXC Private WS

src/exchange/
├── private_ws_base.cpp
├── upbit/private_websocket.cpp
├── binance/private_websocket.cpp
├── bithumb/private_websocket.cpp
└── mexc/private_websocket.cpp

proto/                               # MEXC Protobuf 정의
└── mexc_private.proto
```

---

## ✅ 완료 조건 체크리스트

### Public WebSocket (기 구현 완료)

```
[x] WebSocketClientBase 공통 클래스 구현
[x] 업비트 WebSocket (UpbitWebSocket)
  [x] 시세 구독 (ticker)
  [x] 호가 구독 (orderbook)
  [x] 메시지 파싱
[x] 바이낸스 WebSocket (BinanceWebSocket)
  [x] 시세 구독
  [x] 호가 구독
  [x] Combined Stream 지원
[x] 빗썸 WebSocket (BithumbWebSocket)
  [x] 시세 구독
  [x] 호가 구독
[x] MEXC WebSocket (MEXCWebSocket)
  [x] 시세 구독
  [x] 호가 구독
[x] 공통 기능
  [x] SSL/TLS 연결
  [x] 자동 재연결 (지수 백오프)
  [x] PING/PONG 처리
  [x] Lock-Free Queue 이벤트 전달
  [x] 통계 수집
[x] 통합 테스트 (4개 거래소 동시 연결)
```

### Private WebSocket (미구현)

```
[ ] PrivateWebSocketBase 공통 클래스 구현
  [ ] 인증 인터페이스 (JWT 헤더 / WS 메시지 / listenKey)
  [ ] OrderUpdate 공통 구조체 변환
  [ ] SPSC Queue로 OrderManager에 전달
[ ] 업비트 Private WS
  [ ] JWT (HS512) 인증 + 핸드셰이크 헤더
  [ ] myOrder 구독 + 파싱
  [ ] myAsset 구독 + 파싱
[ ] 빗썸 Private WS
  [ ] JWT (HS256) 인증 + 핸드셰이크 헤더
  [ ] myOrder 구독 + 파싱
  [ ] myAsset 구독 + 파싱
[ ] 바이낸스 Private WS
  [ ] userDataStream.subscribe.signature (HMAC-SHA256)
  [ ] executionReport 파싱
  [ ] outboundAccountPosition 파싱
[ ] MEXC Private WS
  [ ] listenKey 발급/갱신/종료 (REST)
  [ ] Protobuf 디시리얼라이제이션 (protoc 코드 생성)
  [ ] spot@private.orders.v3.api 파싱
  [ ] spot@private.account.v3.api 파싱
[ ] 공통 기능
  [ ] 자동 재연결 + 재인증
  [ ] Keep-alive (Ping/listenKey 갱신)
  [ ] OrderUpdate → SPSC Queue 전달
[ ] 테스트
```

---

## 🔗 의존 관계

```
TASK_01 (프로젝트 셋업) 완료 필요
```

---

## 📎 다음 태스크

완료 후: TASK_03_order_api.md
