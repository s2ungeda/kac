#pragma once

#include "arbitrage/common/error.hpp"
#include <string>
#include <chrono>
#include <mutex>
#include <atomic>
#include <memory>
#include <functional>
#include <thread>
#include <map>

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

// 환율 조회 클래스
class FXRateService {
public:
    FXRateService();
    ~FXRateService();
    
    FXRateService(const FXRateService&) = delete;
    FXRateService& operator=(const FXRateService&) = delete;
    
    // 초기화
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
    
    // HTTP GET (기존 HttpClient 사용)
    Result<std::string> http_get(const std::string& url, 
                                  const std::map<std::string, std::string>& headers = {});
    
private:
    mutable std::mutex mutex_;
    FXRate cached_rate_;
    
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> refresh_thread_;
    
    FXRateCallback on_changed_;
    
    // 캐시 유효 기간
    static constexpr auto CACHE_TTL = std::chrono::seconds(60);
    
    std::shared_ptr<class SimpleLogger> logger_;
};

}  // namespace arbitrage