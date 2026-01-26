#pragma once

#include <string>
#include <optional>
#include <chrono>
#include <array>
#include <cstdint>
#include <vector>
#include <map>
#include <cstring>

namespace arbitrage {

// =============================================================================
// Low-Latency 설계 원칙
// =============================================================================
// 1. Cache-line Awareness: alignas(64) 적용
// 2. Zero-copy: 고정 크기 배열 사용 (std::string 대신 char[])
// 3. Deterministic: 동적 할당 최소화
// =============================================================================

constexpr size_t CACHE_LINE_SIZE = 64;
constexpr size_t MAX_SYMBOL_LEN = 16;   // "KRW-XRP", "XRPUSDT" 등
constexpr size_t MAX_ORDER_ID_LEN = 48; // 주문 ID 최대 길이
constexpr size_t MAX_MESSAGE_LEN = 128; // 에러 메시지 최대 길이
constexpr size_t MAX_ORDERBOOK_DEPTH = 20; // 호가창 깊이

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

// =============================================================================
// 시세 데이터 (Cache-line aligned, 64 bytes)
// =============================================================================
struct alignas(CACHE_LINE_SIZE) Ticker {
    Exchange exchange;                    // 1 byte
    char symbol[MAX_SYMBOL_LEN];          // 16 bytes
    uint8_t _pad1[7];                     // 7 bytes padding
    double price{0.0};                    // 8 bytes
    double bid{0.0};                      // 8 bytes (최우선 매수호가)
    double ask{0.0};                      // 8 bytes (최우선 매도호가)
    double volume_24h{0.0};               // 8 bytes
    int64_t timestamp_us{0};              // 8 bytes (microseconds since epoch)
    // Total: 64 bytes = 1 cache line

    // 헬퍼 함수
    void set_symbol(const char* s) {
        std::strncpy(symbol, s, MAX_SYMBOL_LEN - 1);
        symbol[MAX_SYMBOL_LEN - 1] = '\0';
    }

    void set_symbol(const std::string& s) {
        set_symbol(s.c_str());
    }

    std::string get_symbol() const {
        return std::string(symbol);
    }

    void set_timestamp_now() {
        auto now = std::chrono::system_clock::now();
        timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
    }

    std::chrono::system_clock::time_point get_timestamp() const {
        return std::chrono::system_clock::time_point(
            std::chrono::microseconds(timestamp_us));
    }

    double mid_price() const {
        return (bid + ask) / 2.0;
    }

    double spread() const {
        return ask - bid;
    }

    double spread_pct() const {
        double mid = mid_price();
        return mid > 0.0 ? (ask - bid) / mid * 100.0 : 0.0;
    }
};
static_assert(sizeof(Ticker) == 64, "Ticker must be exactly 64 bytes");

// =============================================================================
// 호가 레벨 (16 bytes)
// =============================================================================
struct PriceLevel {
    double price{0.0};     // 8 bytes
    double quantity{0.0};  // 8 bytes
};
static_assert(sizeof(PriceLevel) == 16, "PriceLevel must be 16 bytes");

// =============================================================================
// 호가창 (Cache-line aligned, 고정 크기)
// Zero-copy를 위해 std::vector 대신 고정 배열 사용
// =============================================================================
struct alignas(CACHE_LINE_SIZE) OrderBook {
    Exchange exchange;                              // 1 byte
    char symbol[MAX_SYMBOL_LEN];                    // 16 bytes
    uint8_t bid_count{0};                           // 1 byte
    uint8_t ask_count{0};                           // 1 byte
    uint8_t _pad[5];                                // 5 bytes padding
    int64_t timestamp_us{0};                        // 8 bytes
    PriceLevel bids[MAX_ORDERBOOK_DEPTH];           // 320 bytes (매수, 내림차순)
    PriceLevel asks[MAX_ORDERBOOK_DEPTH];           // 320 bytes (매도, 오름차순)
    // Total: 672 bytes (11 cache lines)

    // 헬퍼 함수
    void set_symbol(const char* s) {
        std::strncpy(symbol, s, MAX_SYMBOL_LEN - 1);
        symbol[MAX_SYMBOL_LEN - 1] = '\0';
    }

    void set_symbol(const std::string& s) {
        set_symbol(s.c_str());
    }

    void clear() {
        bid_count = 0;
        ask_count = 0;
    }

    void add_bid(double price, double qty) {
        if (bid_count < MAX_ORDERBOOK_DEPTH) {
            bids[bid_count++] = {price, qty};
        }
    }

    void add_ask(double price, double qty) {
        if (ask_count < MAX_ORDERBOOK_DEPTH) {
            asks[ask_count++] = {price, qty};
        }
    }

    double best_bid() const {
        return bid_count > 0 ? bids[0].price : 0.0;
    }

    double best_ask() const {
        return ask_count > 0 ? asks[0].price : 0.0;
    }

    double mid_price() const {
        return (best_bid() + best_ask()) / 2.0;
    }

    double spread() const {
        return best_ask() - best_bid();
    }
};

// =============================================================================
// 주문 요청 (Cache-line aligned, 64 bytes)
// =============================================================================
struct alignas(CACHE_LINE_SIZE) OrderRequest {
    Exchange exchange;                        // 1 byte
    OrderSide side;                           // 1 byte
    OrderType type;                           // 1 byte
    uint8_t _pad1[5];                         // 5 bytes padding
    char symbol[MAX_SYMBOL_LEN];              // 16 bytes
    double quantity{0.0};                     // 8 bytes
    double price{0.0};                        // 8 bytes (0 = Market 주문)
    char client_order_id[MAX_ORDER_ID_LEN];   // 48 bytes (선택, 비어있으면 자동생성)
    // Total: 88 bytes -> padded to 128 bytes (2 cache lines)

    void set_symbol(const char* s) {
        std::strncpy(symbol, s, MAX_SYMBOL_LEN - 1);
        symbol[MAX_SYMBOL_LEN - 1] = '\0';
    }

    void set_symbol(const std::string& s) {
        set_symbol(s.c_str());
    }

    void set_client_order_id(const char* id) {
        std::strncpy(client_order_id, id, MAX_ORDER_ID_LEN - 1);
        client_order_id[MAX_ORDER_ID_LEN - 1] = '\0';
    }

    bool is_market_order() const {
        return type == OrderType::Market || price <= 0.0;
    }
};

// =============================================================================
// 주문 결과 (Cache-line aligned, 128 bytes)
// =============================================================================
struct alignas(CACHE_LINE_SIZE) OrderResult {
    char order_id[MAX_ORDER_ID_LEN];          // 48 bytes
    OrderStatus status{OrderStatus::Pending}; // 1 byte
    uint8_t _pad1[7];                         // 7 bytes padding
    double filled_qty{0.0};                   // 8 bytes
    double avg_price{0.0};                    // 8 bytes
    double commission{0.0};                   // 8 bytes
    int64_t timestamp_us{0};                  // 8 bytes
    char message[MAX_MESSAGE_LEN];            // 128 bytes (에러 메시지)
    // Total: 216 bytes -> padded to 256 bytes (4 cache lines)

    void set_order_id(const char* id) {
        std::strncpy(order_id, id, MAX_ORDER_ID_LEN - 1);
        order_id[MAX_ORDER_ID_LEN - 1] = '\0';
    }

    void set_order_id(const std::string& id) {
        set_order_id(id.c_str());
    }

    void set_message(const char* msg) {
        std::strncpy(message, msg, MAX_MESSAGE_LEN - 1);
        message[MAX_MESSAGE_LEN - 1] = '\0';
    }

    void set_message(const std::string& msg) {
        set_message(msg.c_str());
    }

    bool is_filled() const {
        return status == OrderStatus::Filled;
    }

    bool is_failed() const {
        return status == OrderStatus::Failed || status == OrderStatus::Canceled;
    }
};

// =============================================================================
// 잔고 (Cache-line aligned, 64 bytes)
// =============================================================================
struct alignas(CACHE_LINE_SIZE) Balance {
    char currency[MAX_SYMBOL_LEN];    // 16 bytes
    double available{0.0};            // 8 bytes
    double locked{0.0};               // 8 bytes
    uint8_t _pad[32];                 // 32 bytes padding
    // Total: 64 bytes = 1 cache line

    void set_currency(const char* c) {
        std::strncpy(currency, c, MAX_SYMBOL_LEN - 1);
        currency[MAX_SYMBOL_LEN - 1] = '\0';
    }

    void set_currency(const std::string& c) {
        set_currency(c.c_str());
    }

    double total() const { return available + locked; }
};
static_assert(sizeof(Balance) == 64, "Balance must be exactly 64 bytes");

// 시간 타입 별칭
using Duration = std::chrono::microseconds;
using TimePoint = std::chrono::system_clock::time_point;
using SteadyTimePoint = std::chrono::steady_clock::time_point;

// 김프 매트릭스 타입
using PremiumMatrix = std::array<std::array<double, 4>, 4>;

}  // namespace arbitrage