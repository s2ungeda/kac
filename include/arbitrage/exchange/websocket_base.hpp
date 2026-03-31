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
#include <queue>
#include <variant>
#include <simdjson.h>
#include "arbitrage/common/types.hpp"
#include "arbitrage/common/lockfree_queue.hpp"
#include "arbitrage/common/spin_wait.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/simd_json_parser.hpp"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace arbitrage {

// =============================================================================
// 거래소별 필드 매핑 (파싱 코드 공통화용)
// =============================================================================

struct TickerFieldMap {
    std::string_view symbol_key;
    std::string_view price_key;
    std::string_view bid_key;
    std::string_view ask_key;
    std::string_view volume_key;
    std::string_view timestamp_key;    // empty = use current time
    int64_t ts_multiplier = 1000;      // ms→us
    bool bid_ask_fallback = false;     // bid/ask <= 0 시 price로 대체
};

struct TradeFieldMap {
    std::string_view symbol_key;
    std::string_view price_key;
    std::string_view volume_key;
    std::string_view timestamp_key;    // empty = use current time
    int64_t ts_multiplier = 1000;
};

struct OrderBookFieldMap {
    std::string_view symbol_key;       // empty = fallback_symbol 사용
    std::string_view timestamp_key;    // empty = use current time
    int64_t ts_multiplier = 1000;

    enum Format { OBJECTS, TUPLES } format = OBJECTS;

    // OBJECTS: units[].{ask_price, ask_size, bid_price, bid_size}
    std::string_view units_key;
    std::string_view ask_price_key, ask_size_key;
    std::string_view bid_price_key, bid_size_key;
    bool filter_zero = false;          // price <= 0 항목 무시

    // TUPLES: bids/asks[].[price, qty]
    std::string_view bids_key, asks_key;
};

// WebSocket 이벤트 타입
struct WebSocketEvent {
    enum class Type { 
        Ticker, 
        OrderBook, 
        Trade, 
        Connected, 
        Disconnected, 
        Error 
    };
    
    Type type;
    Exchange exchange;
    std::variant<Ticker, OrderBook> data;
    std::string error_message;
    
    // 생성자들
WebSocketEvent() = default;
    
    // Ticker 이벤트 생성자
    WebSocketEvent(Type t, Exchange ex, const Ticker& tick, const std::string& err = "")
        : type(t), exchange(ex), data(tick), error_message(err) {}
    
    // OrderBook 이벤트 생성자    
    WebSocketEvent(Type t, Exchange ex, const OrderBook& ob, const std::string& err = "")
        : type(t), exchange(ex), data(ob), error_message(err) {}
    
    // Error/기타 이벤트 생성자
    WebSocketEvent(Type t, Exchange ex, const std::string& err = "")
        : type(t), exchange(ex), error_message(err) {}
    
    // 편의 함수
    bool is_ticker() const { return type == Type::Ticker; }
    bool is_trade() const { return type == Type::Trade; }
    bool is_orderbook() const { return type == Type::OrderBook; }
    
    const Ticker& ticker() const { 
        return std::get<Ticker>(data); 
    }
    const OrderBook& orderbook() const { 
        return std::get<OrderBook>(data); 
    }
};

// WebSocket 베이스 클래스
class WebSocketClientBase : public std::enable_shared_from_this<WebSocketClientBase> {
public:
    WebSocketClientBase(net::io_context& ioc, ssl::context& ctx, Exchange exchange);
    virtual ~WebSocketClientBase();
    
    // 연결 관리
    void connect(const std::string& host, const std::string& port, const std::string& target);
    void disconnect();
    bool is_connected() const { return connected_.load(); }
    
    // 이벤트 콜백 (옵션)
    using EventCallback = std::function<void(const WebSocketEvent&)>;
    void on_event(EventCallback cb) { event_callback_ = std::move(cb); }
    
    // 이벤트 큐 (Lock-Free, 메인 스레드에서 폴링)
    LockFreeQueue<WebSocketEvent>& event_queue() { return event_queue_; }
    
    // 통계 (atomic 기반 lock-free)
    struct Stats {
        std::atomic<uint64_t> messages_received{0};
        std::atomic<uint64_t> bytes_received{0};
        std::atomic<uint64_t> reconnect_count{0};
        std::chrono::steady_clock::time_point connected_at;

        // 복사용 스냅샷
        struct Snapshot {
            uint64_t messages_received{0};
            uint64_t bytes_received{0};
            uint64_t reconnect_count{0};
            std::chrono::steady_clock::time_point connected_at;
        };

        Snapshot snapshot() const {
            return {
                messages_received.load(std::memory_order_relaxed),
                bytes_received.load(std::memory_order_relaxed),
                reconnect_count.load(std::memory_order_relaxed),
                connected_at
            };
        }
    };
    Stats::Snapshot get_stats() const;
    
protected:
    // 파생 클래스에서 구현
    virtual std::string build_subscribe_message() = 0;
    virtual void parse_message(const std::string& message) = 0;
    virtual std::chrono::seconds ping_interval() const { 
        return std::chrono::seconds(30); 
    }
    virtual void on_connected() {}
    
    // 이벤트 발행 (파생 클래스에서 호출)
    void emit_event(WebSocketEvent&& evt);

    // 메시지 전송 (파생 클래스에서 호출 가능)
    void send_message(const std::string& message);

    // 공통 파싱 메서드 (FieldMap 기반)
    Ticker make_ticker(simdjson::dom::element data, const TickerFieldMap& map);
    OrderBook make_orderbook(simdjson::dom::element data, const OrderBookFieldMap& map,
                             std::string_view fallback_symbol = "");
    Ticker make_trade(simdjson::dom::element data, const TradeFieldMap& map);
    
private:
    // Boost.Beast 핸들러
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_close(beast::error_code ec);
    void on_ping_timer(beast::error_code ec);
    void on_reconnect_timer(beast::error_code ec);
    
    void do_read();
    void do_write();
    void do_ping();
    void schedule_reconnect();
    void fail(beast::error_code ec, char const* what);
    
protected:
    Exchange exchange_;
    std::shared_ptr<SimpleLogger> logger_;
    
private:
    void reset_websocket();  // 재연결을 위한 WebSocket 스트림 재생성

    net::io_context& ioc_;
    ssl::context& ctx_;
    tcp::resolver resolver_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer buffer_;
    net::steady_timer ping_timer_;
    net::steady_timer reconnect_timer_;
    
    std::string host_, port_, target_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> should_reconnect_{true};
    int reconnect_count_{0};
    
    EventCallback event_callback_;
    LockFreeQueue<WebSocketEvent> event_queue_{4096};
    
    // Write queue (SpinLock 사용 - Lock-Free 호환)
    std::queue<std::string> write_queue_;
    mutable SpinLock write_lock_;
    bool writing_{false};

    // 통계 (atomic 기반, mutex 불필요)
    Stats stats_;
};

}  // namespace arbitrage