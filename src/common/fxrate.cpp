#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/http_client.hpp"
#include "arbitrage/common/json.hpp"
#include <regex>
#include <thread>
#include <fstream>
#include <chrono>

// 네이티브 C++ 크롤러 함수 선언
namespace arbitrage {
    Result<double> fetch_from_investing_native();
}

namespace arbitrage {

FXRateService::FXRateService()
    : logger_(Logger::create("FXRate")) {
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
    // {"rate": 1320.5, "source": "exchangerate-api", "timestamp": "...", "timestamp_unix": 1234567890}
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
        
        // source 추출 (옵션)
        size_t source_pos = content.find("\"source\":\"");
        if (source_pos != std::string::npos) {
            size_t source_start = source_pos + 10;
            size_t source_end = content.find("\"", source_start);
            std::string source = content.substr(source_start, source_end - source_start);
            logger_->info("Got rate {} from {} (via file)", rate, source);
        }
        
        return Ok(std::move(rate));
        
    } catch (const std::exception& e) {
        return Err<double>(ErrorCode::ParseError, "Failed to parse rate");
    }
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
    
    // 4. 캐시된 데이터 사용 (최후의 수단)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cached_rate_.is_valid()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                rate.timestamp - cached_rate_.timestamp).count();

            // 5분 이내의 캐시는 사용 가능
            if (age < 300) {
                logger_->warn("Using cached rate ({} seconds old)", age);
                FXRate cached_copy = cached_rate_;
                cached_copy.source = cached_copy.source + " (cached)";
                return Ok(std::move(cached_copy));
            }
        }
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