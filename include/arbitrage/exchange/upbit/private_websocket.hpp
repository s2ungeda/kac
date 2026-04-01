#pragma once

#include "arbitrage/exchange/websocket_base.hpp"
#include "arbitrage/common/types.hpp"
#include "arbitrage/common/lockfree_queue.hpp"
#include <simdjson.h>
#include <functional>
#include <string>
#include <vector>

namespace arbitrage {

// =============================================================================
// 업비트 Private WebSocket — myOrder 실시간 수신
// =============================================================================
// 엔드포인트: wss://api.upbit.com/websocket/v1/private
// 인증: JWT (HS256) in Authorization 헤더
// 구독: [{"ticket":"uuid"},{"type":"myOrder","codes":["KRW-XRP"]},{"format":"SIMPLE"}]
// =============================================================================
class UpbitPrivateWebSocket : public WebSocketClientBase {
public:
    UpbitPrivateWebSocket(net::io_context& ioc, ssl::context& ctx,
                          const std::string& access_key,
                          const std::string& secret_key);

    // 연결 시작 (symbols: ["KRW-XRP"] 형태)
    void start(const std::vector<std::string>& symbols);

    // OrderUpdate 출력 큐 (OrderTracker에서 폴링)
    SPSCQueue<OrderUpdate>& update_queue() { return update_queue_; }

    // 콜백 방식 (큐 대신 사용 가능)
    using OrderUpdateCallback = std::function<void(const OrderUpdate&)>;
    void on_order_update(OrderUpdateCallback cb) { update_callback_ = std::move(cb); }

protected:
    // WebSocketClientBase 오버라이드
    std::string build_subscribe_message() override;
    void parse_message(const std::string& message) override;
    std::chrono::seconds ping_interval() const override { return std::chrono::seconds(30); }

    // Private WS 인증 헤더 설정
    void configure_handshake() override;

private:
    // JWT 토큰 생성 (HS256, query_hash 없음)
    std::string create_jwt_token();

    // myOrder 이벤트 파싱
    void parse_my_order(simdjson::dom::element data);

    // 업비트 state → OrderStatus 매핑
    static OrderStatus map_state(std::string_view state);

    // RuntimeKeyStore 키 이름
    std::string key_name_;
    std::string secret_name_;

    // 구독 심볼
    SymbolList symbols_;

    // OrderUpdate 출력
    SPSCQueue<OrderUpdate> update_queue_{1024};
    OrderUpdateCallback update_callback_;
};

}  // namespace arbitrage
