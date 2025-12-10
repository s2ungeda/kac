# TASK 09: API 유틸리티 (Rate Limiter + simdjson)

## 🎯 목표
거래소 API 호출 제한 준수 및 초고속 JSON 파싱

---

## 📝 Part 1: Rate Limiter (Token Bucket)

### 왜 필요한가?

```
┌─────────────────────────────────────────────────────────────────┐
│  거래소별 Rate Limit 정책                                       │
├─────────────────────────────────────────────────────────────────┤
│  업비트    │ 초당 8회 (주문)  │ 초당 30회 (조회)  │ 계정 단위   │
│  빗썸      │ 초당 10회 (주문) │ 초당 20회 (조회)  │ 계정 단위   │
│  바이낸스  │ 분당 1200회      │ 분당 6000회       │ IP 단위    │
│  MEXC      │ 초당 20회        │ 초당 50회         │ IP 단위    │
└─────────────────────────────────────────────────────────────────┘
```

### Token Bucket 구현

```cpp
#pragma once

#include <atomic>
#include <chrono>

namespace arbitrage {

class TokenBucketRateLimiter {
public:
    TokenBucketRateLimiter(double rate, size_t burst)
        : rate_(rate), burst_(burst), tokens_(burst) {}
    
    // 토큰 획득 시도 (논블로킹)
    bool try_acquire(size_t count = 1) {
        refill();
        double current = tokens_.load(std::memory_order_relaxed);
        if (current >= count) {
            tokens_.fetch_sub(count, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    
    // 토큰 획득 (블로킹)
    void acquire(size_t count = 1) {
        while (!try_acquire(count)) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(1000.0 / rate_)));
        }
    }
    
private:
    void refill() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
        double new_tokens = std::min(tokens_.load() + elapsed * rate_, 
                                     static_cast<double>(burst_));
        tokens_.store(new_tokens, std::memory_order_relaxed);
        last_refill_ = now;
    }
    
    double rate_;
    size_t burst_;
    std::atomic<double> tokens_;
    std::chrono::steady_clock::time_point last_refill_{std::chrono::steady_clock::now()};
};

// Rate Limiter 관리자
class RateLimitManager {
public:
    void acquire(Exchange ex, ApiType type) {
        get_limiter(ex, type)->acquire();
    }
    
    bool try_acquire(Exchange ex, ApiType type) {
        return get_limiter(ex, type)->try_acquire();
    }
    
private:
    TokenBucketRateLimiter* get_limiter(Exchange ex, ApiType type);
    
    // 거래소별 Limiter 맵
    std::unordered_map<Exchange, std::unique_ptr<TokenBucketRateLimiter>> limiters_;
};

inline RateLimitManager& rate_limits() {
    static RateLimitManager instance;
    return instance;
}

}  // namespace arbitrage
```

---

## 📝 Part 2: simdjson 고성능 JSON 파싱

### 왜 simdjson인가?

```
┌─────────────────────────────────────────────────────────────────┐
│  JSON 파싱 라이브러리 비교                                      │
├─────────────────────────────────────────────────────────────────┤
│  nlohmann::json  │ ~200MB/s  │ 힙 할당 많음   │ 사용 쉬움     │
│  RapidJSON       │ ~500MB/s  │ 중간           │ 중간          │
│  simdjson ★      │ ~3GB/s    │ 최소 (Zero-Copy)│ 중간         │
└─────────────────────────────────────────────────────────────────┘

simdjson: SIMD 명령어(AVX2/AVX-512) 활용, 15배 빠름
```

### simdjson 래퍼

```cpp
#pragma once

#include "simdjson.h"

namespace arbitrage {

class JsonParser {
public:
    // 파싱 (On-Demand)
    Result<simdjson::ondemand::document> parse(std::string_view json) {
        auto error = parser_.iterate(json).get(doc_);
        if (error) {
            return Err<simdjson::ondemand::document>(
                ErrorCode::ParseError, simdjson::error_message(error));
        }
        return Ok(std::move(doc_));
    }
    
private:
    simdjson::ondemand::parser parser_;
    simdjson::ondemand::document doc_;
};

// 스레드 로컬 파서
inline JsonParser& thread_local_parser() {
    thread_local JsonParser parser;
    return parser;
}

}  // namespace arbitrage
```

### 거래소 시세 파싱 예시

```cpp
// 업비트 시세 파싱
Result<Ticker> parse_upbit_ticker(std::string_view json) {
    auto& parser = thread_local_parser();
    auto doc_result = parser.parse(json);
    if (!doc_result) return Err<Ticker>(doc_result.error());
    
    auto& doc = *doc_result;
    
    Ticker ticker;
    ticker.exchange = Exchange::Upbit;
    ticker.symbol = std::string(doc["code"].get_string().value());
    ticker.price = doc["trade_price"].get_double().value();
    ticker.volume = doc["acc_trade_volume_24h"].get_double().value();
    ticker.timestamp = doc["timestamp"].get_int64().value();
    
    return Ok(std::move(ticker));
}

// 바이낸스 시세 파싱
Result<Ticker> parse_binance_ticker(std::string_view json) {
    auto& parser = thread_local_parser();
    auto doc_result = parser.parse(json);
    if (!doc_result) return Err<Ticker>(doc_result.error());
    
    auto& doc = *doc_result;
    
    Ticker ticker;
    ticker.exchange = Exchange::Binance;
    ticker.symbol = std::string(doc["s"].get_string().value());
    ticker.price = std::stod(std::string(doc["c"].get_string().value()));
    ticker.volume = std::stod(std::string(doc["v"].get_string().value()));
    
    return Ok(std::move(ticker));
}
```

---

## ⚠️ simdjson 주의사항

### 문자열 수명 (중요!)

```cpp
// ❌ 위험: 원본 json_str이 먼저 소멸
std::string_view get_symbol(const std::string& json_str) {
    auto doc = parser.parse(json_str);
    return doc["symbol"].get_string().value();  // 댕글링!
}

// ✅ 안전: 문자열 복사
std::string get_symbol(const std::string& json_str) {
    auto doc = parser.parse(json_str);
    return std::string(doc["symbol"].get_string().value());
}
```

### 스레드 안전성

```cpp
// ❌ 위험: simdjson 파서는 스레드 안전하지 않음
static simdjson::ondemand::parser global_parser;

// ✅ 안전: 스레드별 파서 사용
thread_local simdjson::ondemand::parser local_parser;
```

---

## 📦 설치

### vcpkg.json

```json
{
    "dependencies": ["simdjson"]
}
```

### CMake

```cmake
find_package(simdjson CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE simdjson::simdjson)
```

---

## 🔧 사용 예시

```cpp
// Rate Limiter 사용
void place_order(const OrderRequest& req) {
    // 주문 전 토큰 획득 (블로킹)
    rate_limits().acquire(Exchange::Upbit, ApiType::Order);
    
    // API 호출
    auto result = http_client.post(UPBIT_ORDER_URL, req);
}

// simdjson 사용
void on_websocket_message(const std::string& message) {
    auto ticker = parse_upbit_ticker(message);
    if (ticker) {
        process_ticker(*ticker);
    }
}
```

---

## ✅ 완료 조건 체크리스트

```
□ TokenBucketRateLimiter 구현
□ RateLimitManager (거래소/API별)
□ simdjson 래퍼 (JsonParser)
□ 업비트 시세 파싱
□ 바이낸스 시세 파싱
□ 빗썸 시세 파싱
□ MEXC 시세 파싱
□ 스레드 로컬 파서
□ 단위 테스트
□ 벤치마크 (nlohmann 대비)
```

---

## 🔗 의존 관계

```
TASK_01 (프로젝트 셋업) 완료 필요
```

---

## 📎 다음 태스크

완료 후: TASK_10_fxrate_premium.md
