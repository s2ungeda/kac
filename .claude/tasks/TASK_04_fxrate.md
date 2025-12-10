# TASK 06: 환율 조회 (C++ / libcurl)

## 🎯 목표
실시간 USD/KRW 환율 조회 (libcurl 기반)

---

## ⚠️ 주의사항

```
환율 소스 우선순위:
1. Investing.com (공식 환율) - 권장
2. 한국은행 API
3. Exchange Rate API (폴백)

주의:
- 공휴일/주말 데이터 처리
- Rate Limit 준수
- 캐싱 필수 (1분)
```

---

## 📁 생성할 파일

```
include/arbitrage/common/
└── fxrate.hpp
src/common/
└── fxrate.cpp
```

---

## 📝 상세 구현

### 1. include/arbitrage/common/fxrate.hpp

```cpp
#pragma once

#include "arbitrage/common/error.hpp"
#include <string>
#include <chrono>
#include <mutex>
#include <atomic>
#include <memory>
#include <functional>

namespace arbitrage {

// 환율 데이터
struct FXRate {
    double rate{0.0};          // USD/KRW 환율
    std::string source;        // 소스 (investing, bok, fallback)
    std::chrono::system_clock::time_point timestamp;
    bool is_valid() const { return rate > 0.0; }
};

// 환율 변경 콜백
using FXRateCallback = std::function<void(const FXRate&)>;

// 환율 조회 클래스 (libcurl 기반)
class FXRateService {
public:
    FXRateService();
    ~FXRateService();
    
    FXRateService(const FXRateService&) = delete;
    FXRateService& operator=(const FXRateService&) = delete;
    
    // 초기화 (curl_global_init)
    static void global_init();
    static void global_cleanup();
    
    // 환율 조회 (동기)
    Result<FXRate> fetch();
    
    // 현재 캐시된 환율
    FXRate get_cached() const;
    
    // 자동 갱신 시작/중지
    void start_auto_refresh(std::chrono::seconds interval = std::chrono::seconds(60));
    void stop_auto_refresh();
    
    // 콜백 설정
    void on_rate_changed(FXRateCallback cb) { on_changed_ = std::move(cb); }
    
private:
    // 각 소스에서 환율 조회
    Result<double> fetch_from_investing();
    Result<double> fetch_from_bok();       // 한국은행
    Result<double> fetch_from_fallback();  // Exchange Rate API
    
    // HTTP GET (libcurl)
    Result<std::string> http_get(const std::string& url, 
                                  const std::vector<std::string>& headers = {});
    
private:
    mutable std::mutex mutex_;
    FXRate cached_rate_;
    
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> refresh_thread_;
    
    FXRateCallback on_changed_;
    
    // 캐시 유효 기간
    static constexpr auto CACHE_TTL = std::chrono::seconds(60);
    
    std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace arbitrage
```

### 2. src/common/fxrate.cpp

```cpp
#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/common/logger.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <regex>
#include <thread>

using json = nlohmann::json;

namespace arbitrage {

namespace {
    // curl write callback
    size_t write_callback(char* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append(ptr, size * nmemb);
        return size * nmemb;
    }
}

FXRateService::FXRateService()
    : logger_(Logger::create("fxrate"))
{
}

FXRateService::~FXRateService() {
    stop_auto_refresh();
}

void FXRateService::global_init() {
    curl_global_init(CURL_GLOBAL_ALL);
}

void FXRateService::global_cleanup() {
    curl_global_cleanup();
}

Result<std::string> FXRateService::http_get(
    const std::string& url,
    const std::vector<std::string>& headers
) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return Err<std::string>(ErrorCode::NetworkError, "Failed to init curl");
    }
    
    std::string response;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
    
    // 헤더 설정
    struct curl_slist* curl_headers = nullptr;
    for (const auto& h : headers) {
        curl_headers = curl_slist_append(curl_headers, h.c_str());
    }
    if (curl_headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
    }
    
    CURLcode res = curl_easy_perform(curl);
    
    if (curl_headers) {
        curl_slist_free_all(curl_headers);
    }
    
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return Err<std::string>(ErrorCode::NetworkError, 
            std::string("curl error: ") + curl_easy_strerror(res));
    }
    
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    
    if (http_code != 200) {
        return Err<std::string>(ErrorCode::ApiError, 
            "HTTP " + std::to_string(http_code));
    }
    
    return response;
}

Result<double> FXRateService::fetch_from_investing() {
    // Investing.com에서 환율 조회
    // 실제로는 API 또는 스크래핑 필요
    // 여기서는 간단한 예시
    
    std::string url = "https://api.investing.com/api/financialdata/currencies/exchange-rates";
    
    auto result = http_get(url, {
        "Accept: application/json"
    });
    
    if (!result) {
        return Err<double>(result.error());
    }
    
    try {
        auto j = json::parse(*result);
        // JSON 파싱 (실제 API 응답 형식에 맞게)
        double rate = j["data"]["USD_KRW"]["rate"].get<double>();
        return rate;
    } catch (const std::exception& e) {
        return Err<double>(ErrorCode::ParseError, e.what());
    }
}

Result<double> FXRateService::fetch_from_fallback() {
    // Exchange Rate API (무료)
    std::string url = "https://api.exchangerate-api.com/v4/latest/USD";
    
    auto result = http_get(url);
    
    if (!result) {
        return Err<double>(result.error());
    }
    
    try {
        auto j = json::parse(*result);
        double rate = j["rates"]["KRW"].get<double>();
        return rate;
    } catch (const std::exception& e) {
        return Err<double>(ErrorCode::ParseError, e.what());
    }
}

Result<FXRate> FXRateService::fetch() {
    FXRate rate;
    rate.timestamp = std::chrono::system_clock::now();
    
    // 1. Investing.com 시도
    auto investing_result = fetch_from_investing();
    if (investing_result) {
        rate.rate = *investing_result;
        rate.source = "investing";
        
        std::lock_guard lock(mutex_);
        cached_rate_ = rate;
        
        logger_->info("FX rate from Investing: {}", rate.rate);
        return rate;
    }
    logger_->warn("Investing failed: {}", investing_result.error().message);
    
    // 2. 한국은행 시도
    auto bok_result = fetch_from_bok();
    if (bok_result) {
        rate.rate = *bok_result;
        rate.source = "bok";
        
        std::lock_guard lock(mutex_);
        cached_rate_ = rate;
        
        logger_->info("FX rate from BOK: {}", rate.rate);
        return rate;
    }
    
    // 3. Fallback
    auto fallback_result = fetch_from_fallback();
    if (fallback_result) {
        rate.rate = *fallback_result;
        rate.source = "fallback";
        
        std::lock_guard lock(mutex_);
        cached_rate_ = rate;
        
        logger_->info("FX rate from fallback: {}", rate.rate);
        return rate;
    }
    
    return Err<FXRate>(ErrorCode::NetworkError, "All FX sources failed");
}

FXRate FXRateService::get_cached() const {
    std::lock_guard lock(mutex_);
    return cached_rate_;
}

void FXRateService::start_auto_refresh(std::chrono::seconds interval) {
    if (running_.exchange(true)) {
        return;  // 이미 실행 중
    }
    
    refresh_thread_ = std::make_unique<std::thread>([this, interval]() {
        while (running_) {
            auto result = fetch();
            
            if (result && on_changed_) {
                on_changed_(*result);
            }
            
            // interval 동안 대기 (running_ 체크하며)
            for (int i = 0; i < interval.count() && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
    
    logger_->info("Auto refresh started (interval: {}s)", interval.count());
}

void FXRateService::stop_auto_refresh() {
    running_ = false;
    
    if (refresh_thread_ && refresh_thread_->joinable()) {
        refresh_thread_->join();
    }
    refresh_thread_.reset();
}

Result<double> FXRateService::fetch_from_bok() {
    // 한국은행 API (실제 구현 시 API 키 필요)
    return Err<double>(ErrorCode::NetworkError, "BOK not implemented");
}

}  // namespace arbitrage
```

### 3. 사용 예시

```cpp
#include "arbitrage/common/fxrate.hpp"
#include <iostream>

int main() {
    arbitrage::FXRateService::global_init();
    
    arbitrage::FXRateService fxrate;
    
    // 콜백 설정
    fxrate.on_rate_changed([](const arbitrage::FXRate& rate) {
        std::cout << "FX rate updated: " << rate.rate 
                  << " (source: " << rate.source << ")\n";
    });
    
    // 자동 갱신 시작 (1분 간격)
    fxrate.start_auto_refresh(std::chrono::seconds(60));
    
    // 현재 환율
    auto cached = fxrate.get_cached();
    std::cout << "Current rate: " << cached.rate << "\n";
    
    // 수동 조회
    auto result = fxrate.fetch();
    if (result) {
        std::cout << "Fetched: " << result->rate << "\n";
    }
    
    // ...
    
    fxrate.stop_auto_refresh();
    arbitrage::FXRateService::global_cleanup();
    
    return 0;
}
```

---

## ✅ 완료 조건 체크리스트

```
□ libcurl HTTP GET 구현
□ Investing.com 환율 조회
□ 한국은행 API 조회
□ Fallback 소스 구현
□ 캐싱 (1분)
□ 자동 갱신 스레드
□ 콜백 지원
□ 에러 처리
```

---

## 📎 다음 태스크

완료 후: TASK_07_premium_matrix.md
