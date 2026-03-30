#include "arbitrage/exchange/binance/order.hpp"
#include "arbitrage/common/crypto.hpp"
#include "arbitrage/common/json.hpp"
#include "arbitrage/common/runtime_keystore.hpp"
#include <sstream>
#include <cstring>

namespace arbitrage {

BinanceOrderClient::BinanceOrderClient(const std::string& api_key, const std::string& secret_key)
    : key_name_("binance/api_key/" + std::to_string(reinterpret_cast<uintptr_t>(this)))
    , secret_name_("binance/api_secret/" + std::to_string(reinterpret_cast<uintptr_t>(this)))
    , http_(create_http_client())
{
    // 평문을 RuntimeKeyStore에 암호화 저장
    runtime_keystore().store(key_name_, api_key);
    runtime_keystore().store(secret_name_, secret_key);
    // 이후 api_key/secret_key 파라미터는 호출자 측에서 wipe
}

std::string BinanceOrderClient::sign_request(const std::string& query_string) {
    std::ostringstream params;
    params << query_string;
    if (!query_string.empty()) {
        params << "&";
    }
    params << "timestamp=" << get_timestamp_ms();

    std::string signed_params = params.str();
    std::string signature;

    // 순간 복호화 → 서명 → wipe
    runtime_keystore().with_key(secret_name_, [&](const std::string& secret) {
        signature = hmac_sha256(secret, signed_params);
    });

    return signed_params + "&signature=" + to_hex(signature);
}

Result<HttpResponse> BinanceOrderClient::make_request(
    HttpMethod method,
    const std::string& endpoint,
    const std::string& params)
{
    std::string url = std::string(BASE_URL) + endpoint;

    HttpRequest req;
    req.method = method;

    // 순간 복호화 → 헤더 설정 → wipe
    runtime_keystore().with_key(key_name_, [&](const std::string& key) {
        req.headers["X-MBX-APIKEY"] = key;
    });

    if (!params.empty()) {
        std::string signed_params = sign_request(params);

        if (method == HttpMethod::GET || method == HttpMethod::DELETE) {
            url += "?" + signed_params;
            req.url = url;
        } else {
            req.url = url;
            req.body = signed_params;
            req.headers["Content-Type"] = "application/x-www-form-urlencoded";
        }
    } else {
        req.url = url;
    }

    return http_->request(req);
}

std::string BinanceOrderClient::format_symbol(const char* symbol) const {
    if (std::strcmp(symbol, "XRP") == 0) {
        return "XRPUSDT";
    }
    return std::string(symbol);
}

Result<OrderResult> BinanceOrderClient::place_order(const OrderRequest& req) {
    acquire_rate_limit();

    std::ostringstream params;
    params << "symbol=" << format_symbol(req.symbol);
    params << "&side=" << (req.side == OrderSide::Buy ? "BUY" : "SELL");
    params << "&type=" << (req.type == OrderType::Limit ? "LIMIT" : "MARKET");
    params << "&quantity=" << req.quantity;

    if (req.type == OrderType::Limit) {
        if (req.price > 0.0) {
            params << "&price=" << req.price;
        }
        params << "&timeInForce=GTC";
    }

    if (req.client_order_id[0] != '\0') {
        params << "&newClientOrderId=" << req.client_order_id;
    }

    auto resp = make_request(HttpMethod::POST, "/api/v3/order", params.str());

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
        result.set_order_id(std::to_string(json["orderId"].get<long>()).c_str());

        std::string status = json["status"];
        if (status == "NEW") {
            result.status = OrderStatus::Open;
        } else if (status == "FILLED") {
            result.status = OrderStatus::Filled;
        } else if (status == "PARTIALLY_FILLED") {
            result.status = OrderStatus::PartiallyFilled;
        }

        return Ok(std::move(result));

    } catch (const std::exception& e) {
        return Err<OrderResult>(
            ErrorCode::ParseError,
            std::string("Failed to parse response: ") + e.what()
        );
    }
}

Result<OrderResult> BinanceOrderClient::cancel_order(const std::string& order_id) {
    acquire_rate_limit();

    std::ostringstream params;
    params << "symbol=" << format_symbol("XRP");
    params << "&orderId=" << order_id;

    auto resp = make_request(HttpMethod::DELETE, "/api/v3/order", params.str());

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

Result<OrderResult> BinanceOrderClient::get_order(const std::string& order_id) {
    std::ostringstream params;
    params << "symbol=" << format_symbol("XRP");
    params << "&orderId=" << order_id;

    auto resp = make_request(HttpMethod::GET, "/api/v3/order", params.str());

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

Result<Balance> BinanceOrderClient::get_balance(const std::string& currency) {
    auto resp = make_request(HttpMethod::GET, "/api/v3/account");

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
