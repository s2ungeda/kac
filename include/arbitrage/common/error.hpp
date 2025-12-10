#pragma once

#include <string>
#include <variant>
#include <system_error>

namespace arbitrage {

// 에러 코드
enum class ErrorCode {
    Success = 0,
    
    // 네트워크 에러 (100-199)
    NetworkError = 100,
    ConnectionFailed = 101,
    ConnectionTimeout = 102,
    ConnectionClosed = 103,
    SSLError = 104,
    
    // API 에러 (200-299)
    ApiError = 200,
    InvalidRequest = 201,
    AuthenticationFailed = 202,
    RateLimited = 203,
    InsufficientBalance = 204,
    OrderNotFound = 205,
    ExchangeError = 206,
    
    // 내부 에러 (300-399)
    InternalError = 300,
    ConfigError = 301,
    ParseError = 302,
    InvalidState = 303,
    
    // 비즈니스 에러 (400-499)
    BusinessError = 400,
    PremiumTooLow = 401,
    RiskLimitExceeded = 402,
    DailyLossLimitReached = 403,
};

// 에러 구조체
struct Error {
    ErrorCode code;
    std::string message;
    std::string detail;  // 추가 정보 (선택)
    
    Error() : code(ErrorCode::Success) {}
    Error(ErrorCode c, std::string msg) 
        : code(c), message(std::move(msg)) {}
    Error(ErrorCode c, std::string msg, std::string det)
        : code(c), message(std::move(msg)), detail(std::move(det)) {}
    
    bool ok() const { return code == ErrorCode::Success; }
    operator bool() const { return !ok(); }  // 에러가 있으면 true
};

// Result 타입 (std::variant 기반, C++23 std::expected 대체)
template<typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}
    
    bool has_value() const { 
        return std::holds_alternative<T>(data_); 
    }
    
    bool has_error() const {
        return std::holds_alternative<Error>(data_);
    }
    
    T& value() { 
        return std::get<T>(data_); 
    }
    
    const T& value() const { 
        return std::get<T>(data_); 
    }
    
    Error& error() { 
        return std::get<Error>(data_); 
    }
    
    const Error& error() const { 
        return std::get<Error>(data_); 
    }
    
    operator bool() const { return has_value(); }

private:
    std::variant<T, Error> data_;
};


// void 특수화
template<>
class Result<void> {
public:
    Result() : error_(ErrorCode::Success, "") {}
    Result(Error error) : error_(std::move(error)) {}
    
    bool has_value() const { 
        return error_.code == ErrorCode::Success; 
    }
    
    bool has_error() const {
        return error_.code != ErrorCode::Success;
    }
    
    Error& error() { 
        return error_; 
    }
    
    const Error& error() const { 
        return error_; 
    }
    
    operator bool() const { return has_value(); }

private:
    Error error_;
};

// 성공 반환 헬퍼
template<typename T>
Result<T> Ok(T&& value) {
    return Result<T>(std::forward<T>(value));
}

inline Result<void> Ok() {
    return Result<void>();
}

// 에러 반환 헬퍼
template<typename T>
Result<T> Err(ErrorCode code, const std::string& message) {
    return Result<T>(Error{code, message});
}

template<typename T>
Result<T> Err(const Error& error) {
    return Result<T>(error);
}

}  // namespace arbitrage