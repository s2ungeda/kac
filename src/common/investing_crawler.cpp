#include "arbitrage/common/http_client.hpp"
#include "arbitrage/common/logger.hpp"
#include <regex>
#include <random>
#include <chrono>
#include <thread>

namespace arbitrage {

class InvestingCrawler {
private:
    std::shared_ptr<Logger> logger_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> delay_dist_;
    
    // User-Agent 로테이션
    std::vector<std::string> user_agents_ = {
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:121.0) Gecko/20100101 Firefox/121.0"
    };
    
    size_t user_agent_index_ = 0;
    
public:
    InvestingCrawler() 
        : logger_(Logger::create("InvestingCrawler"))
        , rng_(std::chrono::steady_clock::now().time_since_epoch().count())
        , delay_dist_(0.5, 2.0) {
    }
    
    Result<double> fetch_usdkrw() {
        // HTTP 클라이언트 생성
        auto http = create_http_client();
        
        // 헤더 설정 (브라우저처럼 보이게)
        std::map<std::string, std::string> headers = {
            {"User-Agent", get_next_user_agent()},
            {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8"},
            {"Accept-Language", "ko-KR,ko;q=0.9,en-US;q=0.8,en;q=0.7"},
            {"Accept-Encoding", "gzip, deflate, br"},
            {"DNT", "1"},
            {"Connection", "keep-alive"},
            {"Upgrade-Insecure-Requests", "1"},
            {"Sec-Fetch-Dest", "document"},
            {"Sec-Fetch-Mode", "navigate"},
            {"Sec-Fetch-Site", "none"},
            {"Sec-Fetch-User", "?1"},
            {"Cache-Control", "max-age=0"}
        };
        
        // 랜덤 지연
        random_delay();
        
        // investing.com USD/KRW 페이지 요청
        std::string url = "https://www.investing.com/currencies/usd-krw";
        auto response = http->get(url, headers);
        
        if (!response) {
            logger_->error("Failed to fetch page: {}", response.error().message);
            return Err<double>(response.error());
        }
        
        if (!response.value().is_success()) {
            logger_->error("HTTP error: {}", response.value().status_code);
            return Err<double>(ErrorCode::ApiError, "HTTP " + std::to_string(response.value().status_code));
        }
        
        // HTML 파싱
        std::string html = response.value().body;
        
        // 환율 추출 패턴들 (여러 개 시도)
        std::vector<std::string> patterns = {
            // data-test attribute 패턴
            R"(data-test="instrument-price-last"[^>]*>([0-9,]+\.?[0-9]*)</)",
            // class 기반 패턴
            R"(class="[^"]*instrument-price[^"]*"[^>]*>([0-9,]+\.?[0-9]*)</)",
            // pid 기반 패턴 (investing.com 특유)
            R"(<span[^>]*class="[^"]*pid-650-last[^"]*"[^>]*>([0-9,]+\.?[0-9]*)</)",
            // JSON-LD 구조화 데이터
            R"("price"\s*:\s*"([0-9,]+\.?[0-9]*)")",
            // 일반적인 가격 패턴
            R"(>([0-9]{1,3}(?:,[0-9]{3})*(?:\.[0-9]+)?)<[^>]*(?:KRW|원))"
        };
        
        double rate = 0.0;
        bool found = false;
        
        for (const auto& pattern : patterns) {
            std::regex rate_regex(pattern);
            std::smatch matches;
            
            if (std::regex_search(html, matches, rate_regex)) {
                if (matches.size() > 1) {
                    std::string rate_str = matches[1].str();
                    // 콤마 제거
                    rate_str.erase(std::remove(rate_str.begin(), rate_str.end(), ','), rate_str.end());
                    
                    try {
                        rate = std::stod(rate_str);
                        if (rate > 800 && rate < 2000) { // 합리적인 범위 체크
                            found = true;
                            logger_->info("Found USD/KRW rate: {} (pattern: {})", rate, pattern.substr(0, 30));
                            break;
                        }
                    } catch (...) {
                        continue;
                    }
                }
            }
        }
        
        if (!found) {
            // 디버깅을 위해 HTML 일부 로깅
            logger_->debug("HTML snippet: {}", html.substr(0, 500));
            return Err<double>(ErrorCode::ParseError, "Could not find USD/KRW rate in HTML");
        }
        
        return Ok(rate);
    }
    
private:
    std::string get_next_user_agent() {
        // 라운드 로빈으로 User-Agent 변경
        std::string ua = user_agents_[user_agent_index_];
        user_agent_index_ = (user_agent_index_ + 1) % user_agents_.size();
        return ua;
    }
    
    void random_delay() {
        // 0.5~2초 랜덤 지연
        double delay_seconds = delay_dist_(rng_);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(delay_seconds * 1000))
        );
    }
};

// FXRateService에 통합할 새로운 함수
Result<double> fetch_from_investing_native() {
    static InvestingCrawler crawler;
    return crawler.fetch_usdkrw();
}

} // namespace arbitrage