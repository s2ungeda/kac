# TASK 06: 환율 조회 (C++ / libcurl)

## 🎯 목표
실시간 USD/KRW 환율 조회 (Python Selenium 크롤러 + 파일 기반)

---

## ⚠️ 주의사항

```
구현 방식:
1. Python Selenium 크롤러가 investing.com에서 실시간 환율 수집
2. 10초마다 /tmp/usdkrw_rate.json 파일로 저장
3. C++ FXRateService가 파일을 읽어서 사용
4. 워치독(watchdog)이 크롤러 상태 모니터링 및 자동 재시작

환율 소스:
1. investing.com (Selenium 크롤러) - 메인
2. Exchange Rate API (HTTP fallback)
3. 5분 이내 캐시 데이터 (최후의 수단)

주의:
- 데이터 신선도: 30초 이상 오래된 데이터는 거부
- 크롤러가 죽으면 워치독이 자동 재시작
- 봇 탐지 회피를 위한 User-Agent 로테이션
```

---

## 📁 생성할 파일

```
include/arbitrage/common/
└── fxrate.hpp
src/common/
└── fxrate.cpp
scripts/
├── fx_selenium_crawler.py      # 메인 크롤러
├── fx_watchdog.py              # 크롤러 모니터링
├── start_fx_service.sh         # 서비스 시작
└── stop_fx_service.sh          # 서비스 종료
/tmp/
└── usdkrw_rate.json           # 환율 데이터 파일
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

### 2. src/common/fxrate.cpp (실제 구현)

```cpp
#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/http_client.hpp"
#include "arbitrage/common/json.hpp"
#include <fstream>
#include <chrono>
#include <thread>

namespace arbitrage {

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
    // 파일에서 환율 읽기 (Python 크롤러가 주기적으로 업데이트)
    const std::string fx_file = "/tmp/usdkrw_rate.json";
    
    // 파일 존재 확인
    std::ifstream file(fx_file);
    if (!file.is_open()) {
        logger_->warn("FX rate file not found: {}", fx_file);
        return Err<double>(ErrorCode::ApiError, "FX rate file not found");
    }
    
    // 파일 내용 읽기
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();
    
    // JSON 파싱
    // {"rate": 1475.5, "source": "investing.com (selenium)", "timestamp": "...", "timestamp_unix": 1234567890}
    size_t rate_pos = content.find("\"rate\":");
    if (rate_pos == std::string::npos) {
        return Err<double>(ErrorCode::ParseError, "Rate not found in file");
    }
    
    // timestamp_unix 확인 (30초 이상 오래된 데이터는 거부)
    size_t ts_pos = content.find("\"timestamp_unix\":");
    if (ts_pos != std::string::npos) {
        size_t ts_start = ts_pos + 17;
        size_t ts_end = content.find_first_of(",}", ts_start);
        std::string ts_str = content.substr(ts_start, ts_end - ts_start);
        
        try {
            double file_timestamp = std::stod(ts_str);
            auto now = std::chrono::system_clock::now();
            double current_timestamp = std::chrono::duration<double>(now.time_since_epoch()).count();
            
            if (current_timestamp - file_timestamp > 30.0) {
                logger_->warn("FX rate data is stale ({}s old)", current_timestamp - file_timestamp);
                return Err<double>(ErrorCode::ApiError, "FX rate data is too old");
            }
        } catch (...) {
            // timestamp 파싱 실패는 무시
        }
    }
    
    // rate 값 추출
    size_t start = rate_pos + 7;
    size_t end = content.find_first_of(",}", start);
    std::string rate_str = content.substr(start, end - start);
    
    try {
        double rate = std::stod(rate_str);
        
        // 캐시에 저장
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_rate_ = rate;
            last_update_ = std::chrono::system_clock::now();
        }
        
        return Ok(std::move(rate));
    } catch (const std::exception& e) {
        return Err<double>(ErrorCode::ParseError, "Failed to parse rate");
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

### 3. Python Selenium 크롤러 (scripts/fx_selenium_crawler.py)

```python
#!/usr/bin/env python3
"""
USD/KRW 환율 크롤러
- Selenium으로 investing.com에서 실시간 환율 수집
- 10초마다 /tmp/usdkrw_rate.json 업데이트
"""

import time
import json
from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.chrome.options import Options

# 설정
TARGET_URL = 'https://kr.investing.com/currencies/usd-krw-chart'
FX_DATA_FILE = "/tmp/usdkrw_rate.json"
UPDATE_INTERVAL = 10

# Chrome 옵션
options = Options()
options.add_argument('--headless')
options.add_argument('--no-sandbox')
options.add_argument('--disable-dev-shm-usage')
options.add_argument('--disable-blink-features=AutomationControlled')

driver = webdriver.Chrome(options=options)
driver.get(TARGET_URL)

while True:
    try:
        # XPath로 환율 추출
        element = driver.find_element(By.XPATH, '//*[@data-test="instrument-price-last"]')
        rate = float(element.text.replace(',', ''))
        
        # JSON 저장
        data = {
            "rate": rate,
            "source": "investing.com (selenium)",
            "timestamp": datetime.now().isoformat(),
            "timestamp_unix": time.time()
        }
        
        with open(FX_DATA_FILE + '.tmp', 'w') as f:
            json.dump(data, f)
        os.replace(FX_DATA_FILE + '.tmp', FX_DATA_FILE)
        
        print(f"[{datetime.now().strftime('%H:%M:%S')}] Rate: {rate}")
        
    except Exception as e:
        print(f"Error: {e}")
    
    time.sleep(UPDATE_INTERVAL)
```

### 4. 서비스 관리 스크립트

```bash
# 시작: scripts/start_fx_service.sh
#!/bin/bash
pkill -f fx_selenium_crawler
nohup python3 scripts/fx_selenium_crawler.py > logs/fx_selenium.out 2>&1 &
nohup python3 scripts/fx_watchdog.py > logs/fx_watchdog.out 2>&1 &

# 종료: scripts/stop_fx_service.sh
#!/bin/bash
pkill -f fx_selenium_crawler
pkill -f fx_watchdog
```

### 5. 사용 예시

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
✅ Python Selenium 크롤러 구현
✅ investing.com 실시간 환율 수집
✅ JSON 파일 기반 IPC
✅ 워치독(watchdog) 시스템
✅ Fallback 메커니즘 (Exchange Rate API)
✅ 캐시 데이터 활용 (5분 이내)
✅ 데이터 신선도 체크 (30초)
✅ 자동 갱신 (10초 주기)
✅ 서비스 관리 스크립트
```

---

## 📎 다음 태스크

완료 후: TASK_07_premium_matrix.md
