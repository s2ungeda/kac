#pragma once

#include <string>
#include <optional>
#include <chrono>
#include <array>
#include <cstdint>
#include <vector>
#include <map>

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