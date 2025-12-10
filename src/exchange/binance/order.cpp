#include "arbitrage/exchange/binance/order.hpp"
#include "arbitrage/common/crypto.hpp"
#include "arbitrage/common/json.hpp"
#include <sstream>

namespace arbitrage {

BinanceOrderClient::BinanceOrderClient(const std::string& api_key, const std::string& secret_key)
    : api_key_(api_key)
    , secret_key_(secret_key)
    , http_(create_http_client()) {
}

std::string BinanceOrderClient::sign_request(const std::string& query_string) {
    // timestamp 추가
    std::ostringstream params;
    params << query_string;
    if (!query_string.empty()) {
        params << "&";
    }
    params << "timestamp=" << get_timestamp_ms();
    
    // HMAC-SHA256 서명
    std::string signature = hmac_sha256(secret_key_, params.str());
    
    return params.str() + "&signature=" + to_hex(signature);
}

Result<HttpResponse> BinanceOrderClient::make_request(
    HttpMethod method,
    const std::string& endpoint,
    const std::string& params) 
{
    std::string url = std::string(BASE_URL) + endpoint;
    
    HttpRequest req;
    req.method = method;
    req.headers["X-MBX-APIKEY"] = api_key_;
    
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

std::string BinanceOrderClient::format_symbol(const std::string& symbol) const {
    // XRP -> XRPUSDT
    if (symbol == "XRP") {
        return "XRPUSDT";
    }
    return symbol;
}

Result<OrderResult> BinanceOrderClient::place_order(const OrderRequest& req) {
    // Rate limit 체크
    acquire_rate_limit();
    
    // 파라미터 구성
    std::ostringstream params;
    params << "symbol=" << format_symbol(req.symbol);
    params << "&side=" << (req.side == OrderSide::Buy ? "BUY" : "SELL");
    params << "&type=" << (req.type == OrderType::Limit ? "LIMIT" : "MARKET");
    params << "&quantity=" << req.quantity;
    
    if (req.type == OrderType::Limit) {
        if (req.price.has_value()) {
            params << "&price=" << req.price.value();
        }
        params << "&timeInForce=GTC";  // Good Till Cancel
    }
    
    if (!req.client_order_id.empty()) {
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
    
    // 응답 파싱
    try {
        auto json = nlohmann::json::parse(resp.value().body);
        
        OrderResult result;
        result.order_id = std::to_string(json["orderId"].get<long>());
        
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
    params << "symbol=" << format_symbol("XRP");  // TODO: 심볼 저장 필요
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
    
    OrderResult result;
    result.order_id = order_id;
    result.status = OrderStatus::Canceled;
    
    return Ok(std::move(result));
}

Result<OrderResult> BinanceOrderClient::get_order(const std::string& order_id) {
    std::ostringstream params;
    params << "symbol=" << format_symbol("XRP");  // TODO: 심볼 저장 필요
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
    
    // TODO: 응답 파싱
    OrderResult result;
    result.order_id = order_id;
    result.status = OrderStatus::Open;
    
    return Ok(std::move(result));
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
    
    // TODO: 응답 파싱
    Balance balance;
    balance.currency = currency;
    balance.available = 0.0;
    balance.locked = 0.0;
    
    return Ok(std::move(balance));
}

}  // namespace arbitrage