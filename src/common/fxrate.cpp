#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/http_client.hpp"
#include "arbitrage/common/json.hpp"
#include <regex>
#include <thread>

namespace arbitrage {

FXRateService::FXRateService()
    : logger_(nullptr) {  // TODO: Logger 설정
}

FXRateService::~FXRateService() {
    stop_auto_refresh();
}

void FXRateService::global_init() {
    // HTTP client 초기화 (필요시)
}

void FXRateService::global_cleanup() {
    // HTTP client 정리 (필요시)
}

Result<std::string> FXRateService::http_get(
    const std::string& url,
    const std::map<std::string, std::string>& headers) 
{
    auto http = create_http_client();
    auto response = http->get(url, headers);
    
    if (!response) {
        return Err<std::string>(response.error());
    }
    
    if (response.value().is_success()) {
        return Ok(std::string(response.value().body));
    }
    
    return Err<std::string>(ErrorCode::NetworkError, "HTTP " + std::to_string(response.value().status_code));
}

Result<double> FXRateService::fetch_from_investing() {
    // Free Currency API 사용 (무료, 1000 requests/month)
    std::string url = "https://api.freecurrencyapi.com/v1/latest?apikey=fca_live_xxx&currencies=KRW&base_currency=USD";
    
    auto result = http_get(url, {
        {"Accept", "application/json"}
    });
    
    if (!result) {
        return Err<double>(result.error());
    }
    
    // 응답 형식: {"data":{"KRW":1320.0}}
    // JSON 파서 없이는 파싱 불가
    return Err<double>(ErrorCode::ParseError, "JSON parser not available");
}

Result<double> FXRateService::fetch_from_bok() {
    // 한국수출입은행 API (무료, API 키 불필요)
    std::string authkey = "DEMO_API_KEY";  // 실제로는 무료 신청 필요
    std::string searchdate = "";  // 빈 문자열이면 최신 환율
    std::string data = "AP01";  // 환율
    
    std::string url = "https://www.koreaexim.go.kr/site/program/financial/exchangeJSON"
                      "?authkey=" + authkey + 
                      "&searchdate=" + searchdate + 
                      "&data=" + data;
    
    auto result = http_get(url, {});
    
    if (!result) {
        return Err<double>(result.error());
    }
    
    // JSON 파싱 필요 - 실제 구현 필요
    // [{"cur_unit":"USD","cur_nm":"미국 달러","deal_bas_r":"1,320.00"}]
    // 현재는 JSON 파서가 없으므로 임시로 에러 반환
    return Err<double>(ErrorCode::ParseError, "JSON parser not available");
}

Result<double> FXRateService::fetch_from_fallback() {
    // Exchange Rate API (무료)
    std::string url = "https://api.exchangerate-api.com/v4/latest/USD";
    
    auto result = http_get(url, {});
    
    if (!result) {
        return Err<double>(result.error());
    }
    
    // 응답 예시: {"rates":{"KRW":1320.5},"base":"USD","date":"2024-12-10"}
    // JSON 파서 없이 간단한 문자열 파싱
    std::string body = result.value();
    size_t krw_pos = body.find("\"KRW\":");
    if (krw_pos == std::string::npos) {
        return Err<double>(ErrorCode::ParseError, "KRW rate not found in response");
    }
    
    size_t start = krw_pos + 6;  // "KRW": 다음 위치
    size_t end = body.find_first_of(",}", start);
    if (end == std::string::npos) {
        return Err<double>(ErrorCode::ParseError, "Invalid JSON format");
    }
    
    std::string rate_str = body.substr(start, end - start);
    try {
        double rate = std::stod(rate_str);
        return Ok(std::move(rate));
    } catch (const std::exception& e) {
        return Err<double>(ErrorCode::ParseError, "Failed to parse rate: " + rate_str);
    }
}

Result<FXRate> FXRateService::fetch() {
    FXRate rate;
    rate.timestamp = std::chrono::system_clock::now();
    
    // 1. Investing.com 시도
    auto investing_result = fetch_from_investing();
    if (investing_result) {
        rate.rate = investing_result.value();
        rate.source = "investing";
        
        std::lock_guard<std::mutex> lock(mutex_);
        cached_rate_ = rate;
        
        return Ok(std::move(rate));
    }
    
    // 2. 한국은행 시도
    auto bok_result = fetch_from_bok();
    if (bok_result) {
        rate.rate = bok_result.value();
        rate.source = "bok";
        
        std::lock_guard<std::mutex> lock(mutex_);
        cached_rate_ = rate;
        
        return Ok(std::move(rate));
    }
    
    // 3. Fallback
    auto fallback_result = fetch_from_fallback();
    if (fallback_result) {
        rate.rate = fallback_result.value();
        rate.source = "fallback";
        
        std::lock_guard<std::mutex> lock(mutex_);
        cached_rate_ = rate;
        
        return Ok(std::move(rate));
    }
    
    return Err<FXRate>(ErrorCode::ApiError, "모든 API 소스 실패");
}

FXRate FXRateService::get_cached() const {
    std::lock_guard<std::mutex> lock(mutex_);
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
                on_changed_(result.value());
            }
            
            // interval 동안 대기 (running_ 체크하며)
            for (int i = 0; i < interval.count() && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
}

void FXRateService::stop_auto_refresh() {
    running_ = false;
    
    if (refresh_thread_ && refresh_thread_->joinable()) {
        refresh_thread_->join();
    }
    refresh_thread_.reset();
}

}  // namespace arbitrage