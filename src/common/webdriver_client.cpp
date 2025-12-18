#include "arbitrage/common/http_client.hpp"
#include "arbitrage/common/logger.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <random>
#include <chrono>
#include <thread>

namespace arbitrage {

class WebDriverClient {
private:
    std::shared_ptr<Logger> logger_;
    std::string session_id_;
    int port_ = 9515;
    pid_t driver_pid_ = -1;
    std::mt19937 rng_;
    std::uniform_int_distribution<int> delay_dist_;
    
    // User-Agent 로테이션
    std::vector<std::string> user_agents_ = {
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36"
    };
    
public:
    WebDriverClient() 
        : logger_(Logger::create("WebDriver"))
        , rng_(std::chrono::steady_clock::now().time_since_epoch().count())
        , delay_dist_(8000, 13000) {  // 8-13초
    }
    
    ~WebDriverClient() {
        cleanup();
    }
    
    bool start_driver() {
        // ChromeDriver 프로세스 시작
        driver_pid_ = fork();
        
        if (driver_pid_ == 0) {
            // 자식 프로세스
            char port_str[32];
            snprintf(port_str, sizeof(port_str), "--port=%d", port_);
            
            // stdout/stderr 리다이렉트
            freopen("/dev/null", "w", stdout);
            freopen("/dev/null", "w", stderr);
            
            execl("/usr/local/bin/chromedriver", "chromedriver", port_str, nullptr);
            exit(1);
        }
        
        // ChromeDriver 시작 대기
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // 세션 생성
        return create_session();
    }
    
    bool create_session() {
        auto http = create_http_client();
        
        // Chrome 옵션 설정
        std::string options = R"({
            "capabilities": {
                "firstMatch": [{
                    "browserName": "chrome",
                    "goog:chromeOptions": {
                        "args": [
                            "--headless",
                            "--no-sandbox",
                            "--disable-dev-shm-usage",
                            "--disable-blink-features=AutomationControlled",
                            "--disable-gpu",
                            "--window-size=1920,1080",
                            "--user-agent=)" + user_agents_[rng_() % user_agents_.size()] + R"("
                        ],
                        "excludeSwitches": ["enable-automation"],
                        "useAutomationExtension": false,
                        "prefs": {
                            "credentials_enable_service": false,
                            "profile.password_manager_enabled": false
                        }
                    }
                }]
            }
        })";
        
        std::string url = "http://localhost:" + std::to_string(port_) + "/session";
        auto response = http->post(url, options, {{"Content-Type", "application/json"}});
        
        if (response && response.value().is_success()) {
            // session ID 추출
            std::string body = response.value().body;
            size_t id_pos = body.find("\"sessionId\":\"");
            if (id_pos != std::string::npos) {
                size_t start = id_pos + 13;
                size_t end = body.find("\"", start);
                session_id_ = body.substr(start, end - start);
                logger_->info("WebDriver session created: {}", session_id_);
                return true;
            }
        }
        
        return false;
    }
    
    Result<double> fetch_usdkrw() {
        if (session_id_.empty() && !start_driver()) {
            return Err<double>(ErrorCode::SystemError, "Failed to start WebDriver");
        }
        
        auto http = create_http_client();
        std::string base_url = "http://localhost:" + std::to_string(port_) + "/session/" + session_id_;
        
        // 랜덤 지연 (8-13초)
        int delay_ms = delay_dist_(rng_);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        
        // 1. investing.com 페이지 이동
        std::string nav_data = R"({"url": "https://www.investing.com/currencies/usd-krw"})";
        auto nav_response = http->post(base_url + "/url", nav_data, {{"Content-Type", "application/json"}});
        
        if (!nav_response || !nav_response.value().is_success()) {
            return Err<double>(ErrorCode::NetworkError, "Failed to navigate");
        }
        
        // 페이지 로드 대기
        std::this_thread::sleep_for(std::chrono::seconds(3));
        
        // 2. JavaScript로 스크롤 (인간처럼)
        std::string scroll_script = R"({
            "script": "window.scrollBy(0, Math.random() * 300 + 100);",
            "args": []
        })";
        http->post(base_url + "/execute/sync", scroll_script, {{"Content-Type", "application/json"}});
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        
        // 3. 환율 요소 찾기 (여러 선택자 시도)
        std::vector<std::string> selectors = {
            "[data-test='instrument-price-last']",
            ".instrument-price_last__KQzyA",
            "span[class*='instrument-price']",
            ".pid-650-last"
        };
        
        for (const auto& selector : selectors) {
            std::string find_data = R"({"using": "css selector", "value": ")" + selector + R"("})";
            auto elem_response = http->post(base_url + "/element", find_data, {{"Content-Type", "application/json"}});
            
            if (elem_response && elem_response.value().is_success()) {
                // element ID 추출
                std::string body = elem_response.value().body;
                size_t elem_pos = body.find("\"element-");
                if (elem_pos != std::string::npos) {
                    size_t start = elem_pos;
                    size_t end = body.find("\"", start + 1);
                    std::string element_id = body.substr(start, end - start);
                    
                    // 텍스트 가져오기
                    auto text_response = http->get(base_url + "/element/" + element_id + "/text", {});
                    
                    if (text_response && text_response.value().is_success()) {
                        // 값 파싱
                        std::string text_body = text_response.value().body;
                        size_t value_pos = text_body.find("\"value\":\"");
                        if (value_pos != std::string::npos) {
                            size_t val_start = value_pos + 9;
                            size_t val_end = text_body.find("\"", val_start);
                            std::string rate_str = text_body.substr(val_start, val_end - val_start);
                            
                            // 콤마 제거
                            rate_str.erase(std::remove(rate_str.begin(), rate_str.end(), ','), rate_str.end());
                            
                            try {
                                double rate = std::stod(rate_str);
                                if (rate > 800 && rate < 2000) {
                                    logger_->info("Got USD/KRW rate: {}", rate);
                                    return Ok(rate);
                                }
                            } catch (...) {
                                continue;
                            }
                        }
                    }
                }
            }
        }
        
        return Err<double>(ErrorCode::ParseError, "Could not extract rate");
    }
    
    void cleanup() {
        if (!session_id_.empty()) {
            auto http = create_http_client();
            std::string url = "http://localhost:" + std::to_string(port_) + "/session/" + session_id_;
            http->del(url, {});
            session_id_.clear();
        }
        
        if (driver_pid_ > 0) {
            kill(driver_pid_, SIGTERM);
            waitpid(driver_pid_, nullptr, 0);
            driver_pid_ = -1;
        }
    }
};

// FXRateService와 통합
static std::unique_ptr<WebDriverClient> g_webdriver_client;

Result<double> fetch_from_webdriver() {
    if (!g_webdriver_client) {
        g_webdriver_client = std::make_unique<WebDriverClient>();
    }
    return g_webdriver_client->fetch_usdkrw();
}

void cleanup_webdriver() {
    g_webdriver_client.reset();
}

} // namespace arbitrage