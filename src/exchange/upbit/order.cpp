#include "arbitrage/exchange/upbit/order.hpp"
#include "arbitrage/common/crypto.hpp"
#include "arbitrage/common/json.hpp"
#include "arbitrage/common/runtime_keystore.hpp"
#include <sstream>

namespace arbitrage {

UpbitOrderClient::UpbitOrderClient(const std::string& access_key, const std::string& secret_key)
    : key_name_("upbit/access_key/" + std::to_string(reinterpret_cast<uintptr_t>(this)))
    , secret_name_("upbit/secret_key/" + std::to_string(reinterpret_cast<uintptr_t>(this)))
    , http_(create_http_client())
{
    // 평문을 RuntimeKeyStore에 암호화 저장
    runtime_keystore().store(key_name_, access_key);
    runtime_keystore().store(secret_name_, secret_key);
}

std::string UpbitOrderClient::create_query_hash(const std::string& query_string) {
    return sha512(query_string);
}

std::string UpbitOrderClient::create_jwt_token(const std::string& query_string) {
    nlohmann::json header;
    header["alg"] = "HS256";
    header["typ"] = "JWT";

    nlohmann::json payload;
    payload["nonce"] = generate_uuid();
    payload["timestamp"] = get_timestamp_ms();

    // 순간 복호화 → access_key 설정 → wipe
    runtime_keystore().with_key(key_name_, [&](const std::string& access_key) {
        payload["access_key"] = access_key;
    });

    if (!query_string.empty()) {
        payload["query_hash"] = create_query_hash(query_string);
        payload["query_hash_alg"] = "SHA512";
    }

    std::string encoded_header = base64url_encode(header.dump());
    std::string encoded_payload = base64url_encode(payload.dump());
    std::string message = encoded_header + "." + encoded_payload;

    std::string signature;
    // 순간 복호화 → 서명 → wipe
    runtime_keystore().with_key(secret_name_, [&](const std::string& secret) {
        signature = hmac_sha256(secret, message);
    });

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
    if (std::strcmp(symbol, "XRP") == 0) {
        return "KRW-XRP";
    }
    return std::string(symbol);
}

Result<OrderResult> UpbitOrderClient::place_order(const OrderRequest& req) {
    acquire_rate_limit();

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
        if (req.side == OrderSide::Buy && req.price > 0.0) {
            params << "&price=" << (req.price * req.quantity);
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

    try {
        auto json = nlohmann::json::parse(resp.value().body);

        OrderResult result;
        result.set_order_id(json["uuid"].get<std::string>().c_str());
        result.status = OrderStatus::Pending;

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

    return Err<Balance>(
        ErrorCode::NotImplemented,
        "Response parsing not implemented"
    );
}

}  // namespace arbitrage
