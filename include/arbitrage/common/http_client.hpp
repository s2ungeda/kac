#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include "arbitrage/common/error.hpp"

namespace arbitrage {

// HTTP 메서드
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE
};

// HTTP 응답
struct HttpResponse {
    int status_code{0};
    std::string body;
    std::map<std::string, std::string> headers;
    std::chrono::milliseconds elapsed_time{0};
    
    bool is_success() const {
        return status_code >= 200 && status_code < 300;
    }
};

// HTTP 요청 옵션
struct HttpRequest {
    HttpMethod method{HttpMethod::GET};
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::chrono::milliseconds timeout{3000};  // 3초 기본 타임아웃
};

// HTTP 클라이언트 인터페이스
class HttpClient {
public:
    virtual ~HttpClient() = default;
    
    // 동기 요청
    virtual Result<HttpResponse> request(const HttpRequest& req) = 0;
    
    // 편의 메서드
    virtual Result<HttpResponse> get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}
    ) = 0;
    
    virtual Result<HttpResponse> post(
        const std::string& url,
        const std::string& body,
        const std::map<std::string, std::string>& headers = {}
    ) = 0;
    
    virtual Result<HttpResponse> del(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}
    ) = 0;
};

// libcurl 기반 구현
class CurlHttpClient : public HttpClient {
public:
    CurlHttpClient();
    ~CurlHttpClient();
    
    Result<HttpResponse> request(const HttpRequest& req) override;
    
    Result<HttpResponse> get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}
    ) override;
    
    Result<HttpResponse> post(
        const std::string& url,
        const std::string& body,
        const std::map<std::string, std::string>& headers = {}
    ) override;
    
    Result<HttpResponse> del(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}
    ) override;
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// HTTP 클라이언트 생성
std::unique_ptr<HttpClient> create_http_client();

}  // namespace arbitrage