#include <cpprest/http_client.h>
#include <cpprest/json.h>
#include <process.h>
#include <random>
#include <chrono>
#include <thread>

namespace arbitrage {

class ChromeDriverCrawler {
private:
    std::string chromedriver_path_ = "/usr/bin/chromedriver";
    std::string session_id_;
    int port_ = 9515;
    pid_t driver_pid_ = -1;
    
public:
    ChromeDriverCrawler() {}
    ~ChromeDriverCrawler() {
        cleanup();
    }
    
    bool start() {
        // ChromeDriver 프로세스 시작
        driver_pid_ = fork();
        if (driver_pid_ == 0) {
            // 자식 프로세스에서 ChromeDriver 실행
            char port_str[16];
            sprintf(port_str, "--port=%d", port_);
            execl(chromedriver_path_.c_str(), "chromedriver", port_str, nullptr);
            exit(1);
        }
        
        // ChromeDriver 시작 대기
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // 새 세션 생성
        web::http::client::http_client client(
            web::uri_builder("http://localhost")
                .set_port(port_)
                .to_uri()
        );
        
        // Chrome 옵션 설정
        web::json::value desired_caps;
        web::json::value chrome_options;
        web::json::value args = web::json::value::array();
        
        args[0] = web::json::value::string("--headless");
        args[1] = web::json::value::string("--no-sandbox");
        args[2] = web::json::value::string("--disable-dev-shm-usage");
        args[3] = web::json::value::string("--disable-blink-features=AutomationControlled");
        args[4] = web::json::value::string("--user-agent=Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        
        chrome_options["args"] = args;
        
        // WebDriver 속성 숨기기
        web::json::value prefs;
        prefs["webdriver.chrome.driver"] = web::json::value::null();
        chrome_options["prefs"] = prefs;
        
        desired_caps["capabilities"]["firstMatch"][0]["browserName"] = web::json::value::string("chrome");
        desired_caps["capabilities"]["firstMatch"][0]["goog:chromeOptions"] = chrome_options;
        
        try {
            auto response = client.request(
                web::http::methods::POST,
                "/session",
                desired_caps.serialize(),
                "application/json"
            ).get();
            
            if (response.status_code() == web::http::status_codes::OK) {
                auto json = response.extract_json().get();
                session_id_ = json["value"]["sessionId"].as_string();
                return true;
            }
        } catch (...) {
            return false;
        }
        
        return false;
    }
    
    Result<double> fetch_usdkrw() {
        if (session_id_.empty()) {
            if (!start()) {
                return Err<double>(ErrorCode::SystemError, "Failed to start ChromeDriver");
            }
        }
        
        web::http::client::http_client client(
            web::uri_builder("http://localhost")
                .set_port(port_)
                .to_uri()
        );
        
        try {
            // investing.com 페이지로 이동
            web::json::value nav_data;
            nav_data["url"] = web::json::value::string("https://www.investing.com/currencies/usd-krw");
            
            auto nav_response = client.request(
                web::http::methods::POST,
                "/session/" + session_id_ + "/url",
                nav_data.serialize(),
                "application/json"
            ).get();
            
            // 페이지 로드 대기
            std::this_thread::sleep_for(std::chrono::seconds(3));
            
            // 환율 요소 찾기
            web::json::value find_elem;
            find_elem["using"] = web::json::value::string("css selector");
            find_elem["value"] = web::json::value::string("[data-test='instrument-price-last']");
            
            auto elem_response = client.request(
                web::http::methods::POST,
                "/session/" + session_id_ + "/element",
                find_elem.serialize(),
                "application/json"
            ).get();
            
            if (elem_response.status_code() == web::http::status_codes::OK) {
                auto elem_json = elem_response.extract_json().get();
                std::string element_id = elem_json["value"]["element-6066-11e4-a52e-4f735466cecf"].as_string();
                
                // 텍스트 가져오기
                auto text_response = client.request(
                    web::http::methods::GET,
                    "/session/" + session_id_ + "/element/" + element_id + "/text"
                ).get();
                
                if (text_response.status_code() == web::http::status_codes::OK) {
                    auto text_json = text_response.extract_json().get();
                    std::string rate_str = text_json["value"].as_string();
                    
                    // 콤마 제거 후 파싱
                    rate_str.erase(std::remove(rate_str.begin(), rate_str.end(), ','), rate_str.end());
                    double rate = std::stod(rate_str);
                    
                    return Ok(rate);
                }
            }
            
        } catch (const std::exception& e) {
            return Err<double>(ErrorCode::NetworkError, e.what());
        }
        
        return Err<double>(ErrorCode::ParseError, "Failed to extract rate");
    }
    
    void cleanup() {
        if (!session_id_.empty()) {
            // 세션 종료
            web::http::client::http_client client(
                web::uri_builder("http://localhost")
                    .set_port(port_)
                    .to_uri()
            );
            
            client.request(
                web::http::methods::DELETE,
                "/session/" + session_id_
            ).wait();
        }
        
        if (driver_pid_ > 0) {
            kill(driver_pid_, SIGTERM);
            waitpid(driver_pid_, nullptr, 0);
        }
    }
};

} // namespace arbitrage