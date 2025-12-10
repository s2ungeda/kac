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

## ⚠️ 국내 거래소 주문 방식 (참고)

```
┌─────────────────────────────────────────────────────────────────┐
│  업비트/빗썸 통신 방식                                          │
├─────────────────────────────────────────────────────────────────┤
│  시세 수신      : WebSocket (이 태스크)                         │
│  호가 수신      : WebSocket (이 태스크)                         │
│  ─────────────────────────────────────────────────────────────  │
│  주문 실행      : REST API (TASK_03)                           │
│  주문 체결 통보 : WebSocket (myTrade 구독)                     │
└─────────────────────────────────────────────────────────────────┘
```

---

## ✅ 완료 조건 체크리스트

```
□ WebSocketClientBase 공통 클래스 구현
□ 업비트 WebSocket (UpbitWebSocket)
  □ 시세 구독 (ticker)
  □ 호가 구독 (orderbook)
  □ 메시지 파싱
□ 바이낸스 WebSocket (BinanceWebSocket)
  □ 시세 구독
  □ 호가 구독
  □ Combined Stream 지원
□ 빗썸 WebSocket (BithumbWebSocket)
  □ 시세 구독
  □ 호가 구독
□ MEXC WebSocket (MEXCWebSocket)
  □ 시세 구독
  □ 호가 구독
□ 공통 기능
  □ SSL/TLS 연결
  □ 자동 재연결 (지수 백오프)
  □ PING/PONG 처리
  □ Lock-Free Queue 이벤트 전달
  □ 통계 수집
□ 통합 테스트 (4개 거래소 동시 연결)
```

---

## 🔗 의존 관계

```
TASK_01 (프로젝트 셋업) 완료 필요
```

---

## 📎 다음 태스크

완료 후: TASK_03_order_api.md
