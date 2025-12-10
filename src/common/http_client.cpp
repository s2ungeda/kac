#include "arbitrage/common/http_client.hpp"
#include <chrono>
#include <curl/curl.h>
#include <sstream>
#include <mutex>

namespace arbitrage {

// libcurl 콜백 함수
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t totalSize = size * nmemb;
    userp->append((char*)contents, totalSize);
    return totalSize;
}

// CurlHttpClient::Impl - libcurl 실제 구현
class CurlHttpClient::Impl {
private:
    CURL* curl_;
    
public:
    Impl() : curl_(curl_easy_init()) {
        if (!curl_) {
            throw std::runtime_error("Failed to initialize CURL");
        }
    }
    
    ~Impl() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }
    
    Result<HttpResponse> request(const HttpRequest& req) {
        if (!curl_) {
            return Err<HttpResponse>(ErrorCode::InternalError, "CURL not initialized");
        }
        
        HttpResponse resp;
        std::string response_string;
        auto start_time = std::chrono::steady_clock::now();
        
        // URL 설정
        curl_easy_setopt(curl_, CURLOPT_URL, req.url.c_str());
        
        // HTTP 메서드 설정
        switch (req.method) {
            case HttpMethod::GET:
                curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
                break;
            case HttpMethod::POST:
                curl_easy_setopt(curl_, CURLOPT_POST, 1L);
                curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, req.body.c_str());
                break;
            case HttpMethod::DELETE:
                curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
                break;
            default:
                return Err<HttpResponse>(ErrorCode::InvalidRequest, "Unsupported HTTP method");
        }
        
        // 헤더 설정
        struct curl_slist* headers = nullptr;
        for (const auto& [key, value] : req.headers) {
            std::string header = key + ": " + value;
            headers = curl_slist_append(headers, header.c_str());
        }
        if (headers) {
            curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        }
        
        // 응답 데이터 받기 설정
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_string);
        
        // SSL 검증 (개발 중에는 비활성화 가능)
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
        
        // 타임아웃 설정 (30초)
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);
        
        // 요청 실행
        CURLcode res = curl_easy_perform(curl_);
        
        if (headers) {
            curl_slist_free_all(headers);
        }
        
        if (res != CURLE_OK) {
            return Err<HttpResponse>(ErrorCode::NetworkError, 
                "CURL error: " + std::string(curl_easy_strerror(res)));
        }
        
        // 상태 코드 가져오기
        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
        resp.status_code = static_cast<int>(http_code);
        
        // 응답 본문
        resp.body = response_string;
        
        // 소요 시간 계산
        auto end_time = std::chrono::steady_clock::now();
        resp.elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        
        return Ok(std::move(resp));
    }
};

// CURL 전역 초기화 (한 번만)
static bool curl_global_initialized = false;
static std::mutex curl_init_mutex;

CurlHttpClient::CurlHttpClient() : impl_(std::make_unique<Impl>()) {
    std::lock_guard<std::mutex> lock(curl_init_mutex);
    if (!curl_global_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_global_initialized = true;
    }
}

CurlHttpClient::~CurlHttpClient() = default;

Result<HttpResponse> CurlHttpClient::request(const HttpRequest& req) {
    return impl_->request(req);
}

Result<HttpResponse> CurlHttpClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers) 
{
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.url = url;
    req.headers = headers;
    return request(req);
}

Result<HttpResponse> CurlHttpClient::post(
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& headers) 
{
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.url = url;
    req.body = body;
    req.headers = headers;
    return request(req);
}

Result<HttpResponse> CurlHttpClient::del(
    const std::string& url,
    const std::map<std::string, std::string>& headers) 
{
    HttpRequest req;
    req.method = HttpMethod::DELETE;
    req.url = url;
    req.headers = headers;
    return request(req);
}

std::unique_ptr<HttpClient> create_http_client() {
    return std::make_unique<CurlHttpClient>();
}

}  // namespace arbitrage