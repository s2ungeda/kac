#pragma once

#include "arbitrage/exchange/order_base.hpp"
#include "arbitrage/common/http_client.hpp"
#include <string>
#include <memory>

namespace arbitrage {

class BinanceOrderClient : public OrderClientBase {
public:
    BinanceOrderClient(const std::string& api_key, const std::string& secret_key);

    Result<OrderResult> place_order(const OrderRequest& req) override;
    Result<OrderResult> cancel_order(const std::string& order_id) override;
    Result<OrderResult> get_order(const std::string& order_id) override;
    Result<Balance> get_balance(const std::string& currency) override;

    Exchange exchange() const override { return Exchange::Binance; }
    std::string name() const override { return "Binance"; }

private:
    // HMAC-SHA256 서명 (RuntimeKeyStore에서 순간 복호화)
    std::string sign_request(const std::string& query_string);

    // 요청 헬퍼 (RuntimeKeyStore에서 순간 복호화)
    Result<HttpResponse> make_request(
        HttpMethod method,
        const std::string& endpoint,
        const std::string& params = ""
    );

    // 심볼 변환 (XRP -> XRPUSDT)
    std::string format_symbol(const char* symbol) const;

    // RuntimeKeyStore 키 이름 (암호화 상태, 평문 아님)
    std::string key_name_;
    std::string secret_name_;
    std::unique_ptr<HttpClient> http_;

    static constexpr const char* BASE_URL = "https://api.binance.com";
};

}  // namespace arbitrage