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

    // client_order_id(identifier)로 주문 조회 — 멱등성 확인용
    // place_order가 네트워크 오류/5xx로 실패해 접수 여부가 불명일 때,
    // 재주문 전에 이 메서드로 기존 주문 존재 여부를 확인한다.
    Result<OrderResult> get_order_by_identifier(const std::string& identifier);
    
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

    // GET /v1/order 공통 처리 (uuid= 또는 identifier= 파라미터)
    Result<OrderResult> fetch_order(const std::string& params);
    
    // RuntimeKeyStore 키 이름 (암호화 상태, 평문 아님)
    std::string key_name_;
    std::string secret_name_;
    std::unique_ptr<HttpClient> http_;
    
    static constexpr const char* BASE_URL = "https://api.upbit.com";
};

}  // namespace arbitrage