#include "arbitrage/exchange/upbit/private_websocket.hpp"
#include "arbitrage/common/crypto.hpp"
#include "arbitrage/common/json.hpp"
#include "arbitrage/common/runtime_keystore.hpp"
#include "arbitrage/common/simd_json_parser.hpp"

namespace arbitrage {

// =============================================================================
// 생성자
// =============================================================================

UpbitPrivateWebSocket::UpbitPrivateWebSocket(
    net::io_context& ioc, ssl::context& ctx,
    const std::string& access_key, const std::string& secret_key)
    : WebSocketClientBase(ioc, ctx, Exchange::Upbit)
    , key_name_("upbit/private_ws/access_key/" +
                std::to_string(reinterpret_cast<uintptr_t>(this)))
    , secret_name_("upbit/private_ws/secret_key/" +
                   std::to_string(reinterpret_cast<uintptr_t>(this)))
{
    // 평문을 RuntimeKeyStore에 암호화 저장
    runtime_keystore().store(key_name_, access_key);
    runtime_keystore().store(secret_name_, secret_key);
}

// =============================================================================
// 연결 시작
// =============================================================================

void UpbitPrivateWebSocket::start(const std::vector<std::string>& symbols) {
    symbols_.clear();
    for (const auto& s : symbols) {
        symbols_.add(s);
    }
    connect("api.upbit.com", "443", "/websocket/v1/private");
}

// =============================================================================
// JWT 인증 (WebSocket 핸드셰이크 시)
// =============================================================================

std::string UpbitPrivateWebSocket::create_jwt_token() {
    nlohmann::json header;
    header["alg"] = "HS256";
    header["typ"] = "JWT";

    nlohmann::json payload;
    payload["nonce"] = generate_uuid();
    payload["timestamp"] = get_timestamp_ms();

    // 순간 복호화 → access_key 설정
    runtime_keystore().with_key(key_name_, [&](const std::string& access_key) {
        payload["access_key"] = access_key;
    });

    std::string encoded_header = base64url_encode(header.dump());
    std::string encoded_payload = base64url_encode(payload.dump());
    std::string message = encoded_header + "." + encoded_payload;

    std::string signature;
    runtime_keystore().with_key(secret_name_, [&](const std::string& secret) {
        signature = hmac_sha256(secret, message);
    });

    return message + "." + base64url_encode(signature);
}

void UpbitPrivateWebSocket::configure_handshake() {
    std::string jwt = create_jwt_token();

    set_ws_decorator([jwt = std::move(jwt)](websocket::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "kimchi-arbitrage-cpp/1.0");
        req.set(boost::beast::http::field::authorization, "Bearer " + jwt);
    });
}

// =============================================================================
// 구독 메시지
// =============================================================================

std::string UpbitPrivateWebSocket::build_subscribe_message() {
    // [{"ticket":"uuid"},{"type":"myOrder","codes":["KRW-XRP"]},{"format":"SIMPLE"}]
    nlohmann::json ticket = {{"ticket", generate_uuid()}};

    nlohmann::json codes = nlohmann::json::array();
    for (size_t i = 0; i < symbols_.count; ++i) {
        codes.push_back(symbols_.get(i));
    }

    nlohmann::json subscribe = {
        {"type", "myOrder"},
        {"codes", codes}
    };

    nlohmann::json format = {{"format", "SIMPLE"}};

    nlohmann::json msg = nlohmann::json::array({ticket, subscribe, format});
    return msg.dump();
}

// =============================================================================
// 메시지 파싱
// =============================================================================

void UpbitPrivateWebSocket::parse_message(const std::string& message) {
    auto& parser = thread_local_simd_parser();
    simdjson::dom::element doc;
    if (parser.parse(message).get(doc) != simdjson::SUCCESS) {
        logger_->error("[Upbit Private] SIMD JSON parse error");
        return;
    }

    // SIMPLE 형식: ty 필드로 타입 구분
    if (!simd_has_field(doc, "ty")) {
        return;
    }

    std::string_view type = simd_get_sv(doc["ty"]);

    if (type == "myOrder") {
        parse_my_order(doc);
    }
    // myAsset은 추후 필요 시 추가
}

void UpbitPrivateWebSocket::parse_my_order(simdjson::dom::element data) {
    OrderUpdate update{};
    update.exchange = Exchange::Upbit;

    // 거래소 주문 ID (uuid)
    if (simd_has_field(data, "uid")) {
        update.set_order_id(simd_get_sv(data["uid"]));
    }

    // ★ client_order_id (identifier) — 핵심 매칭 키
    if (simd_has_field(data, "id")) {
        update.set_client_order_id(simd_get_sv(data["id"]));
    }

    // 매수/매도
    if (simd_has_field(data, "ab")) {
        std::string_view ab = simd_get_sv(data["ab"]);
        update.side = (ab == "BID") ? OrderSide::Buy : OrderSide::Sell;
    }

    // 주문 상태
    if (simd_has_field(data, "s")) {
        update.status = map_state(simd_get_sv(data["s"]));
    }

    // 체결 수량 (executed_volume)
    update.filled_qty = simd_get_double_or(data["ev"]);

    // 미체결 수량 (remaining_volume)
    update.remaining_qty = simd_get_double_or(data["rv"]);

    // 평균 체결 가격
    update.avg_price = simd_get_double_or(data["ap"]);

    // 수수료 (paid_fee)
    update.commission = simd_get_double_or(data["pf"]);

    // 최근 체결 가격
    if (simd_has_field(data, "tp")) {
        update.last_fill_price = simd_get_double_or(data["tp"]);
    }

    // 최근 체결 수량
    if (simd_has_field(data, "tv")) {
        update.last_fill_qty = simd_get_double_or(data["tv"]);
    }

    // 메이커 여부
    if (simd_has_field(data, "im")) {
        simdjson::dom::element im_elem;
        if (data["im"].get(im_elem) == simdjson::SUCCESS) {
            bool val;
            if (im_elem.get(val) == simdjson::SUCCESS) {
                update.is_maker = val;
            }
        }
    }

    // 타임스탬프
    if (simd_has_field(data, "tms")) {
        int64_t tms = 0;
        simdjson::dom::element tms_elem;
        if (data["tms"].get(tms_elem) == simdjson::SUCCESS) {
            tms_elem.get(tms);
        }
        update.timestamp_ms = tms;
    }

    logger_->info("[Upbit Private] myOrder: cid={} status={} filled={:.4f} remaining={:.4f}",
                  update.client_order_id,
                  static_cast<int>(update.status),
                  update.filled_qty,
                  update.remaining_qty);

    // 출력: 콜백 또는 큐
    if (update_callback_) {
        update_callback_(update);
    } else {
        update_queue_.push(update);
    }
}

// =============================================================================
// 상태 매핑
// =============================================================================

OrderStatus UpbitPrivateWebSocket::map_state(std::string_view state) {
    if (state == "wait" || state == "watch") {
        return OrderStatus::Open;
    } else if (state == "done") {
        return OrderStatus::Filled;
    } else if (state == "cancel") {
        return OrderStatus::Canceled;
    } else if (state == "trade") {
        // trade 이벤트: 부분 체결 또는 완전 체결
        // remaining_qty로 최종 판단은 호출자가 함
        return OrderStatus::PartiallyFilled;
    }
    return OrderStatus::Pending;
}

}  // namespace arbitrage
