#include "arbitrage/exchange/websocket_base.hpp"
#include <boost/asio/ssl/error.hpp>
#include <openssl/ssl.h>
#include <chrono>

namespace arbitrage {

WebSocketClientBase::WebSocketClientBase(net::io_context& ioc, ssl::context& ctx, Exchange exchange)
    : exchange_(exchange)
    , logger_(Logger::get("WebSocket"))
    , ioc_(ioc)
    , ctx_(ctx)
    , resolver_(ioc)
    , ws_(ioc, ctx)
    , ping_timer_(ioc)
    , reconnect_timer_(ioc) {
}

WebSocketClientBase::~WebSocketClientBase() {
    should_reconnect_ = false;

    // 타이머 취소
    beast::error_code ec;
    ping_timer_.cancel(ec);
    reconnect_timer_.cancel(ec);

    // 동기 방식으로 WebSocket 닫기 (소멸자에서는 async 사용 불가)
    if (ws_.is_open()) {
        ws_.close(websocket::close_code::normal, ec);
    }

    connected_ = false;
}

void WebSocketClientBase::connect(const std::string& host, const std::string& port, const std::string& target) {
    host_ = host;
    port_ = port;
    target_ = target;
    
    logger_->debug("[{}] Connecting to {}:{}{}",
                   exchange_name(exchange_), host, port, target);
    
    // 연결 상태 초기화
    connected_ = false;
    
    // DNS 조회
    logger_->debug("[{}] Starting DNS lookup for {}", exchange_name(exchange_), host);
    resolver_.async_resolve(
        host, port,
        beast::bind_front_handler(
            &WebSocketClientBase::on_resolve,
            shared_from_this()));
}

void WebSocketClientBase::disconnect() {
    should_reconnect_ = false;
    
    if (!ws_.is_open()) {
        return;
    }
    
    // Close the WebSocket Connection
    ws_.async_close(websocket::close_code::normal,
        beast::bind_front_handler(
            &WebSocketClientBase::on_close,
            shared_from_this()));
}

void WebSocketClientBase::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        logger_->error("[{}] DNS resolve failed for {}: {}", 
                       exchange_name(exchange_), host_, ec.message());
        return fail(ec, "resolve");
    }
    
    logger_->debug("[{}] DNS resolved {} to {} endpoints", 
                   exchange_name(exchange_), host_, results.size());
    
    // TCP 타임아웃 설정
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
    
    // 연결
    beast::get_lowest_layer(ws_).async_connect(
        results,
        beast::bind_front_handler(
            &WebSocketClientBase::on_connect,
            shared_from_this()));
}

void WebSocketClientBase::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
    if (ec) {
        logger_->error("[{}] TCP connect failed: {} ({})", 
                       exchange_name(exchange_), ec.message(), ep.address().to_string());
        return fail(ec, "connect");
    }
    
    logger_->debug("[{}] TCP connected to {}", exchange_name(exchange_), ep.address().to_string());
    
    // SSL handshake
    beast::get_lowest_layer(ws_).expires_never();
    
    // 모든 거래소에 SNI 설정 (TLS 연결에 필수)
    logger_->debug("[{}] Setting SNI hostname: {}", exchange_name(exchange_), host_);
    if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())) {
        logger_->error("[{}] Failed to set SNI hostname", exchange_name(exchange_));
    }
    
    ws_.next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(
            &WebSocketClientBase::on_ssl_handshake,
            shared_from_this()));
}

void WebSocketClientBase::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        logger_->error("[{}] SSL handshake failed: {} (category: {})", 
                       exchange_name(exchange_), ec.message(), ec.category().name());
        return fail(ec, "ssl_handshake");
    }
    
    logger_->debug("[{}] SSL handshake completed", exchange_name(exchange_));
    
    // WebSocket handshake
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    
    // 기본 User-Agent만 설정
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) {
            req.set(boost::beast::http::field::user_agent, 
                   "kimchi-arbitrage-cpp/1.0");
        }));
    
    ws_.async_handshake(host_, target_,
        beast::bind_front_handler(
            &WebSocketClientBase::on_handshake,
            shared_from_this()));
}

void WebSocketClientBase::on_handshake(beast::error_code ec) {
    if (ec) {
        logger_->error("[{}] WebSocket handshake failed: {}", 
                       exchange_name(exchange_), ec.message());
        return fail(ec, "handshake");
    }
    
    logger_->debug("[{}] WebSocket handshake completed", exchange_name(exchange_));
    
    connected_ = true;
    reconnect_count_ = 0;
    
    // 통계 업데이트
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.connected_at = std::chrono::steady_clock::now();
    }
    
    logger_->debug("[{}] WebSocket connected", exchange_name(exchange_));
    
    // Connected 이벤트 발행
    WebSocketEvent evt(WebSocketEvent::Type::Connected, exchange_);
    emit_event(std::move(evt));
    
    // 구독 메시지 전송 - MEXC는 on_connected에서 처리하므로 순서 변경
    std::string subscribe_msg = build_subscribe_message();
    if (!subscribe_msg.empty()) {
        logger_->debug("[{}] Sending subscribe message: {}", exchange_name(exchange_), subscribe_msg);
        send_message(subscribe_msg);
    }
    
    // 파생 클래스의 on_connected 호출
    on_connected();
    
    // 읽기 시작
    do_read();
    
    // Ping 타이머 시작
    if (ping_interval().count() > 0) {
        do_ping();
    }
}

void WebSocketClientBase::do_read() {
    ws_.async_read(
        buffer_,
        beast::bind_front_handler(
            &WebSocketClientBase::on_read,
            shared_from_this()));
}

void WebSocketClientBase::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        return fail(ec, "read");
    }
    
    // 통계 업데이트
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.messages_received++;
        stats_.bytes_received += bytes_transferred;
    }
    
    // 메시지 파싱
    std::string message = beast::buffers_to_string(buffer_.data());
    buffer_.consume(bytes_transferred);
    
    try {
        parse_message(message);
    } catch (const std::exception& e) {
        logger_->error("[{}] Parse error: {}", 
                       exchange_name(exchange_), e.what());
    }
    
    // 계속 읽기
    do_read();
}

void WebSocketClientBase::send_message(const std::string& message) {
    net::post(ws_.get_executor(),
        [self = shared_from_this(), message]() {
            bool write_in_progress = false;
            {
                std::lock_guard<std::mutex> lock(self->write_mutex_);
                write_in_progress = !self->write_queue_.empty();
                self->write_queue_.push(message);
            }
            
            if (!write_in_progress) {
                self->do_write();
            }
        });
}

void WebSocketClientBase::do_write() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (write_queue_.empty() || writing_) {
        return;
    }
    
    writing_ = true;
    ws_.async_write(
        net::buffer(write_queue_.front()),
        beast::bind_front_handler(
            &WebSocketClientBase::on_write,
            shared_from_this()));
}

void WebSocketClientBase::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        return fail(ec, "write");
    }
    
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_queue_.pop();
        writing_ = false;
    }
    
    // 다음 메시지 전송
    do_write();
}

void WebSocketClientBase::do_ping() {
    ping_timer_.expires_after(ping_interval());
    ping_timer_.async_wait(
        beast::bind_front_handler(
            &WebSocketClientBase::on_ping_timer,
            shared_from_this()));
}

void WebSocketClientBase::on_ping_timer(beast::error_code ec) {
    if (ec) {
        return;
    }
    
    if (!ws_.is_open()) {
        return;
    }
    
    // Ping 전송
    if (exchange_ == Exchange::MEXC) {
        // MEXC는 JSON 형식의 ping을 원함
        send_message("{\"method\": \"PING\"}");
    } else {
        ws_.async_ping({},
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) {
                    self->logger_->error("[{}] Ping failed: {}",
                                         exchange_name(self->exchange_), 
                                         ec.message());
                }
            });
    }
    
    // 다음 ping 예약
    do_ping();
}

void WebSocketClientBase::on_close(beast::error_code ec) {
    if (ec) {
        logger_->error("[{}] Close failed: {}", 
                       exchange_name(exchange_), ec.message());
    }
    
    connected_ = false;
    
    // Disconnected 이벤트 발행
    WebSocketEvent evt(WebSocketEvent::Type::Disconnected, exchange_);
    emit_event(std::move(evt));
}

void WebSocketClientBase::fail(beast::error_code ec, char const* what) {
    logger_->error("[{}] {} failed: {}", 
                   exchange_name(exchange_), what, ec.message());
    
    connected_ = false;
    
    // Error 이벤트 발행
    WebSocketEvent evt(WebSocketEvent::Type::Error, exchange_, 
                      std::string(what) + ": " + ec.message());
    emit_event(std::move(evt));
    
    // 재연결 스케줄링
    if (should_reconnect_) {
        schedule_reconnect();
    }
}

void WebSocketClientBase::schedule_reconnect() {
    reconnect_count_++;
    
    // 지수 백오프 (최대 60초)
    int delay_seconds = std::min(60, (1 << reconnect_count_));
    
    logger_->debug("[{}] Reconnecting in {} seconds (attempt {})",
                   exchange_name(exchange_), delay_seconds, reconnect_count_);
    
    // 통계 업데이트
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.reconnect_count++;
    }
    
    reconnect_timer_.expires_after(std::chrono::seconds(delay_seconds));
    reconnect_timer_.async_wait(
        beast::bind_front_handler(
            &WebSocketClientBase::on_reconnect_timer,
            shared_from_this()));
}

void WebSocketClientBase::on_reconnect_timer(beast::error_code ec) {
    if (ec || !should_reconnect_) {
        return;
    }
    
    connect(host_, port_, target_);
}

void WebSocketClientBase::emit_event(WebSocketEvent&& evt) {
    // 콜백이 설정되어 있으면 콜백으로 처리하고 큐에는 넣지 않음
    if (event_callback_) {
        event_callback_(evt);
        return;
    }

    // 콜백이 없으면 Lock-Free Queue에 추가
    if (!event_queue_.push(std::move(evt))) {
        logger_->warn("[{}] Event queue full, dropping event",
                      exchange_name(exchange_));
    }
}

WebSocketClientBase::Stats WebSocketClientBase::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

}  // namespace arbitrage