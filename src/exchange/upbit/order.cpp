#include "arbitrage/exchange/upbit/order.hpp"
#include "arbitrage/common/crypto.hpp"
#include "arbitrage/common/json.hpp"
#include <sstream>

namespace arbitrage {

UpbitOrderClient::UpbitOrderClient(const std::string& access_key, const std::string& secret_key)
    : access_key_(access_key)
    , secret_key_(secret_key)
    , http_(create_http_client()) {
}

std::string UpbitOrderClient::create_query_hash(const std::string& query_string) {
    // SHA512 해시
    return sha512(query_string);
}

std::string UpbitOrderClient::create_jwt_token(const std::string& query_string) {
    // JWT 헤더
    nlohmann::json header;
    header["alg"] = "HS256";
    header["typ"] = "JWT";
    
    // JWT 페이로드
    nlohmann::json payload;
    payload["access_key"] = access_key_;
    payload["nonce"] = generate_uuid();
    payload["timestamp"] = get_timestamp_ms();
    
    if (!query_string.empty()) {
        payload["query_hash"] = create_query_hash(query_string);
        payload["query_hash_alg"] = "SHA512";
    }
    
    // Base64URL 인코딩
    std::string encoded_header = base64url_encode(header.dump());
    std::string encoded_payload = base64url_encode(payload.dump());
    
    // 서명
    std::string message = encoded_header + "." + encoded_payload;
    std::string signature = hmac_sha256(secret_key_, message);
    std::string encoded_signature = base64url_encode(signature);
    
    return message + "." + encoded_signature;
}

Result<HttpResponse> UpbitOrderClient::make_request(
    HttpMethod method,
    const std::string& endpoint,
    const std::string& query_string) 
{
    std::string url = std::string(BASE_URL) + endpoint;
    if (!query_string.empty() && method == HttpMethod::GET) {
        url += "?" + query_string;
    }
    
    HttpRequest req;
    req.method = method;
    req.url = url;
    req.headers["Authorization"] = "Bearer " + create_jwt_token(query_string);
    
    if (!query_string.empty() && method == HttpMethod::POST) {
        req.body = query_string;
        req.headers["Content-Type"] = "application/x-www-form-urlencoded";
    }
    
    return http_->request(req);
}

std::string UpbitOrderClient::format_symbol(const char* symbol) const {
    // XRP -> KRW-XRP
    if (std::strcmp(symbol, "XRP") == 0) {
        return "KRW-XRP";
    }
    return std::string(symbol);
}

Result<OrderResult> UpbitOrderClient::place_order(const OrderRequest& req) {
    // Rate limit 체크
    acquire_rate_limit();

    // 파라미터 구성
    std::ostringstream params;
    params << "market=" << format_symbol(req.symbol);
    params << "&side=" << (req.side == OrderSide::Buy ? "bid" : "ask");
    params << "&ord_type=" << (req.type == OrderType::Limit ? "limit" : "market");

    if (req.type == OrderType::Limit) {
        if (req.price > 0.0) {
            params << "&price=" << req.price;
        }
        params << "&volume=" << req.quantity;
    } else {
        // Market 주문은 KRW 금액 또는 수량
        if (req.side == OrderSide::Buy && req.price > 0.0) {
            params << "&price=" << (req.price * req.quantity);  // KRW 금액
        } else {
            params << "&volume=" << req.quantity;
        }
    }

    if (req.client_order_id[0] != '\0') {
        params << "&identifier=" << req.client_order_id;
    }
    
    auto resp = make_request(HttpMethod::POST, "/v1/orders", params.str());
    
    if (!resp) {
        return Err<OrderResult>(resp.error());
    }
    
    if (!resp.value().is_success()) {
        return Err<OrderResult>(
            ErrorCode::ExchangeError,
            "Order failed: " + resp.value().body
        );
    }
    
    // 응답 파싱
    try {
        auto json = nlohmann::json::parse(resp.value().body);

        OrderResult result;
        result.set_order_id(json["uuid"].get<std::string>().c_str());
        result.status = OrderStatus::Pending;  // Upbit은 대기 상태로 시작

        return Ok(std::move(result));
        
    } catch (const std::exception& e) {
        return Err<OrderResult>(
            ErrorCode::ParseError,
            std::string("Failed to parse response: ") + e.what()
        );
    }
}

Result<OrderResult> UpbitOrderClient::cancel_order(const std::string& order_id) {
    acquire_rate_limit();
    
    std::string params = "uuid=" + order_id;
    auto resp = make_request(HttpMethod::DELETE, "/v1/order", params);
    
    if (!resp) {
        return Err<OrderResult>(resp.error());
    }
    
    if (!resp.value().is_success()) {
        return Err<OrderResult>(
            ErrorCode::ExchangeError,
            "Cancel failed: " + resp.value().body
        );
    }
    
    // TODO: 응답 파싱
    return Err<OrderResult>(
        ErrorCode::NotImplemented,
        "Response parsing not implemented"
    );
}

Result<OrderResult> UpbitOrderClient::get_order(const std::string& order_id) {
    std::string params = "uuid=" + order_id;
    auto resp = make_request(HttpMethod::GET, "/v1/order", params);
    
    if (!resp) {
        return Err<OrderResult>(resp.error());
    }
    
    if (!resp.value().is_success()) {
        return Err<OrderResult>(
            ErrorCode::ExchangeError,
            "Get order failed: " + resp.value().body
        );
    }
    
    // TODO: 응답 파싱
    return Err<OrderResult>(
        ErrorCode::NotImplemented,
        "Response parsing not implemented"
    );
}

Result<Balance> UpbitOrderClient::get_balance(const std::string& currency) {
    auto resp = make_request(HttpMethod::GET, "/v1/accounts");
    
    if (!resp) {
        return Err<Balance>(resp.error());
    }
    
    if (!resp.value().is_success()) {
        return Err<Balance>(
            ErrorCode::ExchangeError,
            "Get balance failed: " + resp.value().body
        );
    }
    
    // TODO: 응답 파싱
    return Err<Balance>(
        ErrorCode::NotImplemented,
        "Response parsing not implemented"
    );
}

}  // namespace arbitrage