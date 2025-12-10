# TASK 03: 주문 API (4개 거래소)

## 🎯 목표
libcurl 기반 4개 거래소 REST API 주문 구현

---

## ⚠️ 국내 거래소 통신 방식 (중요!)

```
┌─────────────────────────────────────────────────────────────────┐
│  주문 흐름                                                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   [주문 실행]     REST API  ────────────────────────────────→  │
│                   • 동기 호출, 주문 접수 확인                    │
│                   • 거래소별 인증 방식 다름                      │
│                                                                 │
│   [체결 통보]     WebSocket  ←────────────────────────────────  │
│                   • 실시간 체결 알림                            │
│                   • TASK_02에서 구현                            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## ⚠️ 주의사항

```
필수:
- Rate Limit 준수 (TASK_08 Rate Limiter 사용)
- 체결 통보는 WebSocket으로 수신 (REST 폴링 금지)
- 주문 실패 시 재시도 로직
- 타임아웃 설정 (3초)
```

---

## 📁 생성할 파일

```
include/arbitrage/exchange/
├── order_base.hpp              # 공통 주문 인터페이스
├── upbit/order.hpp
├── binance/order.hpp
├── bithumb/order.hpp
└── mexc/order.hpp

src/exchange/
├── order_base.cpp
├── upbit/order.cpp
├── binance/order.cpp
├── bithumb/order.cpp
└── mexc/order.cpp

tests/unit/exchange/
└── order_test.cpp
```

---

## 📊 거래소별 차이점

| 항목 | 업비트 | 바이낸스 | 빗썸 | MEXC |
|------|--------|----------|------|------|
| **인증** | JWT (HS256) | HMAC-SHA256 | HMAC-SHA512 | HMAC-SHA256 |
| **Rate Limit** | 초당 8회 | 분당 1200회 | 초당 10회 | 초당 20회 |
| **단위** | 계정 | IP | 계정 | IP |
| **심볼** | KRW-XRP | XRPUSDT | XRP | XRPUSDT |
| **주문 URL** | /v1/orders | /api/v3/order | /trade/place | /api/v3/order |

---

## 📝 공통 인터페이스

### order_base.hpp

```cpp
#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/error.hpp"
#include "arbitrage/common/rate_limiter.hpp"
#include <string>
#include <memory>

namespace arbitrage {

// 주문 요청
struct OrderRequest {
    std::string symbol;
    OrderSide side;         // Buy, Sell
    OrderType type;         // Market, Limit
    double quantity;
    double price;           // Limit 주문 시
    std::string client_id;  // 클라이언트 주문 ID (선택)
};

// 주문 결과
struct OrderResult {
    std::string order_id;
    std::string client_id;
    OrderStatus status;
    double filled_quantity;
    double avg_price;
    std::string message;
};

// 공통 주문 인터페이스
class OrderClientBase {
public:
    virtual ~OrderClientBase() = default;
    
    // 주문
    virtual Result<OrderResult> place_order(const OrderRequest& req) = 0;
    virtual Result<OrderResult> cancel_order(const std::string& order_id) = 0;
    virtual Result<OrderResult> get_order(const std::string& order_id) = 0;
    
    // 잔고
    virtual Result<Balance> get_balance(const std::string& currency) = 0;
    
    // 거래소 정보
    virtual Exchange exchange() const = 0;
    virtual std::string name() const = 0;
    
protected:
    // Rate Limiter (공통)
    void acquire_rate_limit() {
        rate_limits().acquire(exchange(), ApiType::Order);
    }
    
    bool try_acquire_rate_limit() {
        return rate_limits().try_acquire(exchange(), ApiType::Order);
    }
};

// 팩토리 함수
std::unique_ptr<OrderClientBase> create_order_client(
    Exchange exchange,
    const std::string& api_key,
    const std::string& secret_key);

}  // namespace arbitrage
```

---

## 📝 거래소별 구현

### 1. 업비트 (Upbit)

```cpp
// upbit/order.hpp
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
    
    std::string access_key_;
    std::string secret_key_;
    std::unique_ptr<HttpClient> http_;
};

// JWT 인증 (HS256)
std::string UpbitOrderClient::create_jwt_token(const std::string& query_string) {
    json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    json payload = {
        {"access_key", access_key_},
        {"nonce", generate_uuid()},
        {"timestamp", get_timestamp_ms()}
    };
    
    if (!query_string.empty()) {
        payload["query_hash"] = create_query_hash(query_string);
        payload["query_hash_alg"] = "SHA512";
    }
    
    // Base64URL 인코딩 및 서명
    return sign_jwt(header, payload, secret_key_);
}
```

### 2. 바이낸스 (Binance)

```cpp
// binance/order.hpp
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
    // HMAC-SHA256 서명
    std::string sign_request(const std::string& query_string);
    
    std::string api_key_;
    std::string secret_key_;
    std::unique_ptr<HttpClient> http_;
};

// 서명 예시
std::string BinanceOrderClient::sign_request(const std::string& query_string) {
    // timestamp 추가
    std::string params = query_string + "&timestamp=" + std::to_string(get_timestamp_ms());
    
    // HMAC-SHA256
    std::string signature = hmac_sha256(secret_key_, params);
    
    return params + "&signature=" + signature;
}
```

### 3. 빗썸 (Bithumb)

```cpp
// bithumb/order.hpp
class BithumbOrderClient : public OrderClientBase {
public:
    BithumbOrderClient(const std::string& api_key, const std::string& secret_key);
    
    Result<OrderResult> place_order(const OrderRequest& req) override;
    Result<OrderResult> cancel_order(const std::string& order_id) override;
    Result<OrderResult> get_order(const std::string& order_id) override;
    Result<Balance> get_balance(const std::string& currency) override;
    
    Exchange exchange() const override { return Exchange::Bithumb; }
    std::string name() const override { return "Bithumb"; }
    
private:
    // HMAC-SHA512 서명
    std::string create_signature(const std::string& endpoint, const std::string& params);
    
    std::string api_key_;
    std::string secret_key_;
    std::unique_ptr<HttpClient> http_;
};

// 빗썸 서명 (특이함)
std::string BithumbOrderClient::create_signature(
    const std::string& endpoint, 
    const std::string& params) 
{
    std::string nonce = std::to_string(get_timestamp_ms());
    std::string message = endpoint + ";" + params + ";" + nonce;
    
    // Base64(HMAC-SHA512)
    return base64_encode(hmac_sha512(secret_key_, message));
}
```

### 4. MEXC

```cpp
// mexc/order.hpp
class MEXCOrderClient : public OrderClientBase {
public:
    MEXCOrderClient(const std::string& api_key, const std::string& secret_key);
    
    Result<OrderResult> place_order(const OrderRequest& req) override;
    Result<OrderResult> cancel_order(const std::string& order_id) override;
    Result<OrderResult> get_order(const std::string& order_id) override;
    Result<Balance> get_balance(const std::string& currency) override;
    
    Exchange exchange() const override { return Exchange::MEXC; }
    std::string name() const override { return "MEXC"; }
    
private:
    std::string sign_request(const std::string& query_string);
    
    std::string api_key_;
    std::string secret_key_;
    std::unique_ptr<HttpClient> http_;
};
```

---

## 🔧 사용 예시

```cpp
#include "arbitrage/exchange/order.hpp"

// 거래소별 주문 클라이언트 생성
auto upbit = create_order_client(Exchange::Upbit, upbit_key, upbit_secret);
auto binance = create_order_client(Exchange::Binance, binance_key, binance_secret);

// 주문 요청
OrderRequest req;
req.symbol = "KRW-XRP";  // 거래소별 심볼 형식 주의
req.side = OrderSide::Buy;
req.type = OrderType::Limit;
req.quantity = 100;
req.price = 850;

// 주문 실행 (Rate Limit 자동 적용)
auto result = upbit->place_order(req);

if (result) {
    std::cout << "주문 성공: " << result->order_id << "\n";
} else {
    std::cerr << "주문 실패: " << result.error().message << "\n";
}
```

---

## ✅ 완료 조건 체크리스트

```
□ OrderClientBase 공통 인터페이스
□ 업비트 주문 (UpbitOrderClient)
  □ JWT 인증 (HS256)
  □ query_hash (SHA512)
  □ 주문/취소/조회/잔고
□ 바이낸스 주문 (BinanceOrderClient)
  □ HMAC-SHA256 서명
  □ 주문/취소/조회/잔고
□ 빗썸 주문 (BithumbOrderClient)
  □ HMAC-SHA512 서명
  □ x-www-form-urlencoded
  □ 주문/취소/조회/잔고
□ MEXC 주문 (MEXCOrderClient)
  □ HMAC-SHA256 서명
  □ 주문/취소/조회/잔고
□ Rate Limiter 통합
□ 에러 처리 및 재시도
□ 단위 테스트
```

---

## 🔗 의존 관계

```
TASK_01 (프로젝트 셋업) 완료 필요
TASK_08 (Rate Limiter) 완료 필요
```

---

## 📎 다음 태스크

완료 후: TASK_04_executor.md
