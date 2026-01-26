#pragma once

#include "arbitrage/exchange/order_base.hpp"
#include "arbitrage/common/http_client.hpp"
#include <string>
#include <memory>

namespace arbitrage {

class UpbitOrderClient : public OrderClientBase {
public:
    UpbitOrderClient(const std::string& access_key, const std::string& secret_key);
    
    Result<OrderResult> place_order(const OrderRequest& req) override;
    Result<OrderResult> cancel_order(const std::string& order_id) override;
    Result<OrderResult> get_order(const std::string& order_id) override;
    Result<Balance> get_balance(const std::string& currency) override;
    
    Exchange exchange() const override { return Exchange::Upbit; }
    std::string name() const override { return "Upbit"; }
    
private:
    // JWT 토큰 생성
    std::string create_jwt_token(const std::string& query_string = "");
    std::string create_query_hash(const std::string& query_string);
    
    // 요청 헬퍼
    Result<HttpResponse> make_request(
        HttpMethod method,
        const std::string& endpoint,
        const std::string& query_string = ""
    );
    
    // 심볼 변환 (XRP -> KRW-XRP)
    std::string format_symbol(const char* symbol) const;
    
    std::string access_key_;
    std::string secret_key_;
    std::unique_ptr<HttpClient> http_;
    
    static constexpr const char* BASE_URL = "https://api.upbit.com";
};

}  // namespace arbitrage