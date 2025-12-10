# TASK 01: 프로젝트 셋업 (C++ / Boost.Beast)

## 🎯 목표
CMake 기반 C++ 프로젝트 구조 수립 및 의존성 설정

---

## ⚠️ 주의사항

```
절대 금지:
- raw new/delete 사용
- 전역 변수
- std::mutex 남용 (Lock-Free 우선)

필수:
- Boost.Beast + Boost.Asio 사용 (WebSocket)
- libcurl 사용 (HTTP REST)
- C++20 표준
- RAII 패턴
- Lock-Free Queue (스레드 간 통신)
```

---

## 📁 생성할 파일

```
kimchi-arbitrage-cpp/
├── CMakeLists.txt
├── cmake/
│   ├── Dependencies.cmake
│   └── CompilerFlags.cmake
├── vcpkg.json
├── include/arbitrage/
│   ├── common/
│   │   ├── types.hpp
│   │   ├── config.hpp
│   │   ├── logger.hpp
│   │   └── error.hpp
│   └── exchange/
│       └── interface.hpp
├── src/
│   ├── main.cpp
│   └── common/
│       ├── config.cpp
│       └── logger.cpp
├── config/
│   └── config.yaml
└── tests/
    └── CMakeLists.txt
```

---

## 📝 상세 구현

### 1. CMakeLists.txt (최상위)

```cmake
cmake_minimum_required(VERSION 3.20)
project(kimchi-arbitrage VERSION 1.0.0 LANGUAGES CXX)

# C++20 필수
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 빌드 타입 기본값
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 출력 디렉토리
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# CMake 모듈 경로
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# 컴파일러 플래그
include(CompilerFlags)

# 의존성
include(Dependencies)

# 헤더 경로
include_directories(${CMAKE_SOURCE_DIR}/include)

# 서브디렉토리
add_subdirectory(src)
add_subdirectory(tests)
```

### 2. cmake/CompilerFlags.cmake

```cmake
# 컴파일러별 플래그 설정

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # GCC / Clang
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        -Werror=return-type
        -fPIC
    )
    
    # Release 최적화
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
    
    # Debug 설정
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")
    
elseif(MSVC)
    # Visual Studio
    add_compile_options(
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
    )
    
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi")
endif()

# 링크 타임 최적화 (Release)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_SUPPORTED)
    if(IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()
```

### 3. cmake/Dependencies.cmake

```cmake
# 의존성 찾기

# OpenSSL (필수)
find_package(OpenSSL REQUIRED)

# Boost.Beast
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBWEBSOCKETS REQUIRED Boost.Beast>=4.0)

# libcurl
find_package(CURL REQUIRED)

# SQLite3
find_package(SQLite3 REQUIRED)

# yaml-cpp
find_package(yaml-cpp REQUIRED)

# spdlog (vcpkg)
find_package(spdlog REQUIRED)

# nlohmann_json (vcpkg)
find_package(nlohmann_json REQUIRED)

# Google Test
find_package(GTest REQUIRED)

# msgpack (선택)
find_package(msgpack-cxx CONFIG)

# 전역 include 경로
include_directories(
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

# 전역 링크 디렉토리
link_directories(
    ${LIBWEBSOCKETS_LIBRARY_DIRS}
)
```

### 4. vcpkg.json

```json
{
    "name": "kimchi-arbitrage",
    "version": "1.0.0",
    "description": "XRP Kimchi Premium Arbitrage System",
    "dependencies": [
        {
            "name": "boost-beast",
            "version>=": "1.83.0"
        },
        {
            "name": "boost-asio",
            "version>=": "1.83.0"
        },
        "curl",
        "openssl",
        "spdlog",
        "simdjson",
        "yaml-cpp",
        "sqlite3",
        "gtest",
        "msgpack-cxx"
    ],
    "builtin-baseline": "2024.01.12"
}
```

> ⚠️ **주요 라이브러리 설치:**
> - **simdjson**: SIMD 기반 고성능 JSON 파싱 (nlohmann::json 대체)
> - **Lock-Free Queue**: rigtorp/SPSCQueue는 헤더 온리이므로 직접 include 또는 git submodule로 추가

### 5. include/arbitrage/common/error.hpp

```cpp
#pragma once

#include <string>
#include <expected>
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

// Result 타입 (C++23 std::expected)
template<typename T>
using Result = std::expected<T, Error>;

// 성공 반환 헬퍼
template<typename T>
Result<T> Ok(T&& value) {
    return Result<T>(std::forward<T>(value));
}

// 에러 반환 헬퍼
template<typename T>
Result<T> Err(ErrorCode code, const std::string& message) {
    return std::unexpected(Error{code, message});
}

template<typename T>
Result<T> Err(const Error& error) {
    return std::unexpected(error);
}

}  // namespace arbitrage
```

### 6. include/arbitrage/common/types.hpp

```cpp
#pragma once

#include <string>
#include <optional>
#include <chrono>
#include <array>
#include <cstdint>

namespace arbitrage {

// 거래소 열거형
enum class Exchange : uint8_t {
    Upbit = 0,
    Bithumb = 1,
    Binance = 2,
    MEXC = 3,
    Count = 4
};

// 거래소 이름 변환
constexpr const char* exchange_name(Exchange ex) {
    switch (ex) {
        case Exchange::Upbit:   return "upbit";
        case Exchange::Bithumb: return "bithumb";
        case Exchange::Binance: return "binance";
        case Exchange::MEXC:    return "mexc";
        default:                return "unknown";
    }
}

// KRW 거래소 여부
constexpr bool is_krw_exchange(Exchange ex) {
    return ex == Exchange::Upbit || ex == Exchange::Bithumb;
}

// 주문 방향
enum class OrderSide : uint8_t {
    Buy,
    Sell
};

// 주문 타입
enum class OrderType : uint8_t {
    Limit,
    Market
};

// 주문 상태
enum class OrderStatus : uint8_t {
    Pending,
    Open,
    PartiallyFilled,
    Filled,
    Canceled,
    Failed
};

// 시세 데이터
struct Ticker {
    Exchange exchange;
    std::string symbol;
    double price{0.0};
    double bid{0.0};         // 최우선 매수호가
    double ask{0.0};         // 최우선 매도호가
    double volume_24h{0.0};
    std::chrono::system_clock::time_point timestamp;
    
    double mid_price() const { 
        return (bid + ask) / 2.0; 
    }
    
    double spread() const {
        return ask - bid;
    }
    
    double spread_pct() const {
        return (ask - bid) / mid_price() * 100.0;
    }
};

// 호가 레벨
struct PriceLevel {
    double price;
    double quantity;
};

// 호가창
struct OrderBook {
    Exchange exchange;
    std::string symbol;
    std::vector<PriceLevel> bids;  // 매수 (내림차순)
    std::vector<PriceLevel> asks;  // 매도 (오름차순)
    std::chrono::system_clock::time_point timestamp;
    
    double best_bid() const { 
        return bids.empty() ? 0.0 : bids[0].price; 
    }
    double best_ask() const { 
        return asks.empty() ? 0.0 : asks[0].price; 
    }
    double mid_price() const {
        return (best_bid() + best_ask()) / 2.0;
    }
};

// 주문 요청
struct OrderRequest {
    Exchange exchange;
    std::string symbol;
    OrderSide side;
    OrderType type;
    double quantity;
    std::optional<double> price;  // Market 주문 시 nullopt
    std::string client_order_id;  // 클라이언트 주문 ID (선택)
};

// 주문 결과
struct OrderResult {
    std::string order_id;
    OrderStatus status;
    double filled_qty{0.0};
    double avg_price{0.0};
    double commission{0.0};
    std::string message;
    std::chrono::system_clock::time_point timestamp;
};

// 잔고
struct Balance {
    std::string currency;
    double available{0.0};
    double locked{0.0};
    
    double total() const { return available + locked; }
};

// 시간 타입 별칭
using Duration = std::chrono::microseconds;
using TimePoint = std::chrono::system_clock::time_point;
using SteadyTimePoint = std::chrono::steady_clock::time_point;

// 김프 매트릭스 타입
using PremiumMatrix = std::array<std::array<double, 4>, 4>;

}  // namespace arbitrage
```

### 7. include/arbitrage/common/config.hpp

```cpp
#pragma once

#include "arbitrage/common/types.hpp"
#include <string>
#include <map>
#include <memory>
#include <mutex>

namespace arbitrage {

// 거래소별 설정
struct ExchangeConfig {
    std::string api_key;
    std::string api_secret;
    std::string passphrase;  // 일부 거래소용
    
    std::string ws_url;
    std::string rest_url;
    
    int rate_limit_per_second{10};
    int rate_limit_per_minute{600};
    
    bool enabled{false};
};

// 전략 설정
struct StrategyConfig {
    double min_premium_pct{3.0};      // 최소 진입 김프
    double max_premium_pct{10.0};     // 최대 진입 김프
    double stop_loss_pct{1.0};        // 손절 기준
    
    double min_order_qty{10.0};       // 최소 주문량 (XRP)
    double max_order_qty{10000.0};    // 최대 주문량
    double max_position{50000.0};     // 최대 포지션
    
    double max_slippage_pct{0.5};     // 최대 슬리피지
    int order_timeout_ms{5000};       // 주문 타임아웃
};

// 리스크 설정
struct RiskConfig {
    double daily_loss_limit_krw{500000.0};  // 일일 손실 한도
    double max_transfer_amount{50000.0};     // 최대 송금량
    int max_concurrent_orders{4};            // 동시 주문 수
    bool kill_switch_enabled{true};          // 킬스위치 활성화
};

// TCP 서버 설정
struct ServerConfig {
    std::string bind_address{"0.0.0.0"};
    int port{9800};
    int max_connections{10};
    std::string auth_token;
};

// 알림 설정
struct AlertConfig {
    bool telegram_enabled{false};
    std::string telegram_token;
    std::string telegram_chat_id;
    
    bool discord_enabled{false};
    std::string discord_webhook;
};

// 전체 설정
class Config {
public:
    // 싱글톤 접근
    static Config& instance();
    
    // 설정 파일 로드
    bool load(const std::string& path);
    
    // 설정 파일 리로드
    bool reload();
    
    // Getter
    const ExchangeConfig& exchange(Exchange ex) const;
    const StrategyConfig& strategy() const { return strategy_; }
    const RiskConfig& risk() const { return risk_; }
    const ServerConfig& server() const { return server_; }
    const AlertConfig& alert() const { return alert_; }
    
    // 설정 파일 경로
    const std::string& config_path() const { return config_path_; }
    
private:
    Config() = default;
    
    mutable std::mutex mutex_;
    std::string config_path_;
    
    std::map<Exchange, ExchangeConfig> exchanges_;
    StrategyConfig strategy_;
    RiskConfig risk_;
    ServerConfig server_;
    AlertConfig alert_;
};

}  // namespace arbitrage
```

### 8. include/arbitrage/common/logger.hpp

```cpp
#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <string>

namespace arbitrage {

class Logger {
public:
    // 초기화 (main에서 한 번 호출)
    static void init(
        const std::string& log_dir = "logs",
        spdlog::level::level_enum console_level = spdlog::level::info,
        spdlog::level::level_enum file_level = spdlog::level::debug
    );
    
    // 로거 생성/조회
    static std::shared_ptr<spdlog::logger> create(const std::string& name);
    static std::shared_ptr<spdlog::logger> get(const std::string& name);
    
    // 기본 로거
    static std::shared_ptr<spdlog::logger> default_logger();
    
    // 종료
    static void shutdown();
    
private:
    static std::vector<spdlog::sink_ptr> sinks_;
    static bool initialized_;
};

// 편의 매크로
#define LOG_TRACE(...)    SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

}  // namespace arbitrage
```

### 9. include/arbitrage/exchange/interface.hpp

```cpp
#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/error.hpp"
#include <functional>
#include <memory>
#include <future>

namespace arbitrage {

// 콜백 타입
using TickerCallback = std::function<void(const Ticker&)>;
using OrderBookCallback = std::function<void(const OrderBook&)>;
using OrderCallback = std::function<void(const OrderResult&)>;

// 거래소 인터페이스
class IExchange {
public:
    virtual ~IExchange() = default;
    
    // 거래소 식별
    virtual Exchange name() const = 0;
    
    // 연결 관리
    virtual Result<void> connect() = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    
    // 구독
    virtual void subscribe_ticker(const std::string& symbol, TickerCallback cb) = 0;
    virtual void subscribe_orderbook(const std::string& symbol, OrderBookCallback cb) = 0;
    virtual void unsubscribe(const std::string& symbol) = 0;
    
    // 주문
    virtual std::future<Result<OrderResult>> place_order(const OrderRequest& req) = 0;
    virtual std::future<Result<OrderResult>> cancel_order(const std::string& order_id) = 0;
    virtual std::future<Result<OrderResult>> get_order(const std::string& order_id) = 0;
    
    // 잔고
    virtual std::future<Result<std::map<std::string, Balance>>> get_balances() = 0;
    
    // RTT 측정
    virtual std::future<Result<Duration>> ping() = 0;
    
    // 이벤트 루프 (blocking)
    virtual void run() = 0;
    
    // 이벤트 루프 (non-blocking, 한 번 실행)
    virtual void poll() = 0;
    
    // 이벤트 루프 중지
    virtual void stop() = 0;
};

// 거래소 팩토리
std::unique_ptr<IExchange> create_exchange(Exchange ex);

}  // namespace arbitrage
```

### 10. src/main.cpp

```cpp
#include "arbitrage/common/config.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/exchange/interface.hpp"
#include <iostream>
#include <csignal>
#include <atomic>

namespace {
    std::atomic<bool> g_running{true};
    
    void signal_handler(int signum) {
        LOG_INFO("Received signal {}, shutting down...", signum);
        g_running = false;
    }
}

int main(int argc, char* argv[]) {
    // 설정 파일 경로
    std::string config_path = "config/config.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }
    
    // 로거 초기화
    arbitrage::Logger::init("logs");
    auto logger = arbitrage::Logger::create("main");
    
    logger->info("Starting Kimchi Arbitrage System (C++)");
    logger->info("Config: {}", config_path);
    
    // 설정 로드
    if (!arbitrage::Config::instance().load(config_path)) {
        logger->error("Failed to load config");
        return 1;
    }
    
    // 시그널 핸들러
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // TODO: 거래소 연결 및 전략 실행
    logger->info("System initialized successfully");
    
    // 메인 루프
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // 정리
    logger->info("Shutting down...");
    arbitrage::Logger::shutdown();
    
    return 0;
}
```

### 11. src/common/config.cpp

```cpp
#include "arbitrage/common/config.hpp"
#include "arbitrage/common/logger.hpp"
#include <yaml-cpp/yaml.h>
#include <fstream>

namespace arbitrage {

Config& Config::instance() {
    static Config instance;
    return instance;
}

bool Config::load(const std::string& path) {
    std::lock_guard lock(mutex_);
    config_path_ = path;
    
    try {
        YAML::Node root = YAML::LoadFile(path);
        
        // 거래소 설정
        if (root["exchanges"]) {
            auto exchanges = root["exchanges"];
            
            auto load_exchange = [&](const std::string& name, Exchange ex) {
                if (exchanges[name]) {
                    auto node = exchanges[name];
                    ExchangeConfig cfg;
                    cfg.enabled = node["enabled"].as<bool>(false);
                    cfg.api_key = node["api_key"].as<std::string>("");
                    cfg.api_secret = node["api_secret"].as<std::string>("");
                    cfg.ws_url = node["ws_url"].as<std::string>("");
                    cfg.rest_url = node["rest_url"].as<std::string>("");
                    cfg.rate_limit_per_second = node["rate_limit_per_second"].as<int>(10);
                    exchanges_[ex] = cfg;
                }
            };
            
            load_exchange("upbit", Exchange::Upbit);
            load_exchange("bithumb", Exchange::Bithumb);
            load_exchange("binance", Exchange::Binance);
            load_exchange("mexc", Exchange::MEXC);
        }
        
        // 전략 설정
        if (root["strategy"]) {
            auto s = root["strategy"];
            strategy_.min_premium_pct = s["min_premium_pct"].as<double>(3.0);
            strategy_.max_premium_pct = s["max_premium_pct"].as<double>(10.0);
            strategy_.min_order_qty = s["min_order_qty"].as<double>(10.0);
            strategy_.max_order_qty = s["max_order_qty"].as<double>(10000.0);
        }
        
        // 리스크 설정
        if (root["risk"]) {
            auto r = root["risk"];
            risk_.daily_loss_limit_krw = r["daily_loss_limit_krw"].as<double>(500000.0);
            risk_.kill_switch_enabled = r["kill_switch_enabled"].as<bool>(true);
        }
        
        // 서버 설정
        if (root["server"]) {
            auto srv = root["server"];
            server_.bind_address = srv["bind_address"].as<std::string>("0.0.0.0");
            server_.port = srv["port"].as<int>(9800);
        }
        
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Config load error: {}", e.what());
        return false;
    }
}

bool Config::reload() {
    return load(config_path_);
}

const ExchangeConfig& Config::exchange(Exchange ex) const {
    std::lock_guard lock(mutex_);
    static ExchangeConfig empty;
    auto it = exchanges_.find(ex);
    return it != exchanges_.end() ? it->second : empty;
}

}  // namespace arbitrage
```

### 12. src/common/logger.cpp

```cpp
#include "arbitrage/common/logger.hpp"
#include <spdlog/sinks/daily_file_sink.h>
#include <filesystem>

namespace arbitrage {

std::vector<spdlog::sink_ptr> Logger::sinks_;
bool Logger::initialized_ = false;

void Logger::init(
    const std::string& log_dir,
    spdlog::level::level_enum console_level,
    spdlog::level::level_enum file_level
) {
    if (initialized_) return;
    
    // 로그 디렉토리 생성
    std::filesystem::create_directories(log_dir);
    
    // 콘솔 싱크
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(console_level);
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
    
    // 파일 싱크 (일별 로테이션)
    auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
        log_dir + "/arbitrage.log", 0, 0  // 자정에 로테이션
    );
    file_sink->set_level(file_level);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [%t] %v");
    
    sinks_ = {console_sink, file_sink};
    
    // 기본 로거 설정
    auto default_logger = std::make_shared<spdlog::logger>("default", sinks_.begin(), sinks_.end());
    default_logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(default_logger);
    
    // flush 정책
    spdlog::flush_on(spdlog::level::warn);
    spdlog::flush_every(std::chrono::seconds(3));
    
    initialized_ = true;
}

std::shared_ptr<spdlog::logger> Logger::create(const std::string& name) {
    auto logger = spdlog::get(name);
    if (logger) return logger;
    
    logger = std::make_shared<spdlog::logger>(name, sinks_.begin(), sinks_.end());
    logger->set_level(spdlog::level::trace);
    spdlog::register_logger(logger);
    
    return logger;
}

std::shared_ptr<spdlog::logger> Logger::get(const std::string& name) {
    return spdlog::get(name);
}

std::shared_ptr<spdlog::logger> Logger::default_logger() {
    return spdlog::default_logger();
}

void Logger::shutdown() {
    spdlog::shutdown();
}

}  // namespace arbitrage
```

### 13. config/config.yaml

```yaml
# 김프 아비트라지 설정 파일

exchanges:
  upbit:
    enabled: true
    api_key: "${UPBIT_API_KEY}"
    api_secret: "${UPBIT_API_SECRET}"
    ws_url: "wss://api.upbit.com/websocket/v1"
    rest_url: "https://api.upbit.com/v1"
    rate_limit_per_second: 10
    rate_limit_per_minute: 600
    
  bithumb:
    enabled: true
    api_key: "${BITHUMB_API_KEY}"
    api_secret: "${BITHUMB_API_SECRET}"
    ws_url: "wss://pubwss.bithumb.com/pub/ws"
    rest_url: "https://api.bithumb.com"
    rate_limit_per_second: 15
    
  binance:
    enabled: true
    api_key: "${BINANCE_API_KEY}"
    api_secret: "${BINANCE_API_SECRET}"
    ws_url: "wss://stream.binance.com:9443/ws"
    rest_url: "https://api.binance.com"
    rate_limit_per_second: 10
    
  mexc:
    enabled: true
    api_key: "${MEXC_API_KEY}"
    api_secret: "${MEXC_API_SECRET}"
    ws_url: "wss://wbs.mexc.com/ws"
    rest_url: "https://api.mexc.com"
    rate_limit_per_second: 10

strategy:
  min_premium_pct: 3.0
  max_premium_pct: 10.0
  stop_loss_pct: 1.0
  min_order_qty: 10.0
  max_order_qty: 10000.0
  max_position: 50000.0

risk:
  daily_loss_limit_krw: 500000.0
  max_transfer_amount: 50000.0
  max_concurrent_orders: 4
  kill_switch_enabled: true

server:
  bind_address: "0.0.0.0"
  port: 9800
  max_connections: 10

alert:
  telegram_enabled: false
  telegram_token: "${TELEGRAM_TOKEN}"
  telegram_chat_id: "${TELEGRAM_CHAT_ID}"
```

### 14. src/CMakeLists.txt

```cmake
# 공통 라이브러리
add_library(common
    common/config.cpp
    common/logger.cpp
)

target_link_libraries(common
    PUBLIC
        spdlog::spdlog
        yaml-cpp::yaml-cpp
)

# 메인 실행 파일
add_executable(arbitrage
    main.cpp
)

target_link_libraries(arbitrage
    PRIVATE
        common
        ${LIBWEBSOCKETS_LIBRARIES}
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite::SQLite3
)
```

---

## ✅ 완료 조건 체크리스트

```
□ CMakeLists.txt 최상위 파일 생성
□ cmake/Dependencies.cmake - 의존성 찾기
□ cmake/CompilerFlags.cmake - 컴파일러 설정
□ vcpkg.json 생성
□ include/arbitrage/common/error.hpp - Result<T> 타입
□ include/arbitrage/common/types.hpp - 공통 타입
□ include/arbitrage/common/config.hpp - 설정 클래스
□ include/arbitrage/common/logger.hpp - 로깅
□ include/arbitrage/exchange/interface.hpp - 거래소 인터페이스
□ src/main.cpp - 엔트리포인트
□ src/common/config.cpp - 설정 구현
□ src/common/logger.cpp - 로깅 구현
□ config/config.yaml - 기본 설정 파일
□ 빌드 성공 (ninja)
□ 실행 시 설정 파일 로드 확인
```

---

## 🔗 의존 관계

```
없음 (첫 번째 태스크)
```

---

## 📎 다음 태스크

완료 후: TASK_02_upbit_websocket.md
