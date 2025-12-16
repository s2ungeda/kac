#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace arbitrage {
namespace mexc {

// Simple protobuf wire format parser
class ProtobufParser {
public:
    struct Field {
        uint32_t tag;
        uint32_t wire_type;
        std::string data;
    };

    static std::vector<Field> parse(const std::string& data);
    
    // Helper functions for common types
    static double parseDouble(const std::string& data);
    static int64_t parseInt64(const std::string& data);
    static uint64_t parseUint64(const std::string& data);
    static std::string parseString(const std::string& data);
    
    // Wire types
    enum WireType {
        VARINT = 0,
        FIXED64 = 1,
        LENGTH_DELIMITED = 2,
        FIXED32 = 5
    };
};

// MEXC specific message parsers
struct MEXCTrade {
    double price;
    double quantity;
    int64_t timestamp;
    int trade_type; // 0=buy, 1=sell
};

struct MEXCOrderBookLevel {
    double price;
    double quantity;
};

struct MEXCOrderBook {
    std::vector<MEXCOrderBookLevel> asks;
    std::vector<MEXCOrderBookLevel> bids;
    int64_t version;
};

struct MEXCTicker {
    std::string symbol;
    double price;
    double volume;
    double high;
    double low;
    double rate; // 24h change rate
};

// Parse MEXC specific messages
std::optional<std::vector<MEXCTrade>> parseAggreDeals(const std::string& data);
std::optional<MEXCOrderBook> parseAggreDepth(const std::string& data);
std::optional<MEXCTicker> parseMiniTicker(const std::string& data);

}  // namespace mexc
}  // namespace arbitrage