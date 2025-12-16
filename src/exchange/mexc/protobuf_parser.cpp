#include "arbitrage/exchange/mexc/protobuf_parser.hpp"
#include <cstring>
#include <stdexcept>

namespace arbitrage {
namespace mexc {

// Varint decoding
static uint64_t decodeVarint(const uint8_t* data, size_t& offset, size_t max_len) {
    uint64_t result = 0;
    int shift = 0;
    
    while (offset < max_len) {
        uint8_t byte = data[offset++];
        result |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return result;
        }
        shift += 7;
        if (shift >= 64) {
            throw std::runtime_error("Varint too large");
        }
    }
    throw std::runtime_error("Unexpected end of varint");
}

std::vector<ProtobufParser::Field> ProtobufParser::parse(const std::string& data) {
    std::vector<Field> fields;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    size_t offset = 0;
    size_t len = data.length();
    
    while (offset < len) {
        // Parse tag
        uint64_t tag_wire = decodeVarint(bytes, offset, len);
        uint32_t tag = tag_wire >> 3;
        uint32_t wire_type = tag_wire & 0x07;
        
        Field field;
        field.tag = tag;
        field.wire_type = wire_type;
        
        // Parse value based on wire type
        switch (wire_type) {
            case VARINT: {
                size_t start = offset;
                decodeVarint(bytes, offset, len);
                field.data = data.substr(start, offset - start);
                break;
            }
            case FIXED64: {
                if (offset + 8 > len) throw std::runtime_error("Unexpected end of data");
                field.data = data.substr(offset, 8);
                offset += 8;
                break;
            }
            case LENGTH_DELIMITED: {
                uint64_t length = decodeVarint(bytes, offset, len);
                if (offset + length > len) throw std::runtime_error("Unexpected end of data");
                field.data = data.substr(offset, length);
                offset += length;
                break;
            }
            case FIXED32: {
                if (offset + 4 > len) throw std::runtime_error("Unexpected end of data");
                field.data = data.substr(offset, 4);
                offset += 4;
                break;
            }
            default:
                throw std::runtime_error("Unknown wire type");
        }
        
        fields.push_back(field);
    }
    
    return fields;
}

double ProtobufParser::parseDouble(const std::string& data) {
    if (data.length() != 8) throw std::runtime_error("Invalid double data");
    double result;
    std::memcpy(&result, data.data(), 8);
    return result;
}

int64_t ProtobufParser::parseInt64(const std::string& data) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    size_t offset = 0;
    uint64_t result = decodeVarint(bytes, offset, data.length());
    // Convert unsigned to signed using zigzag decoding
    return (result >> 1) ^ -(result & 1);
}

uint64_t ProtobufParser::parseUint64(const std::string& data) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    size_t offset = 0;
    return decodeVarint(bytes, offset, data.length());
}

std::string ProtobufParser::parseString(const std::string& data) {
    return data;
}

// Parse MEXC aggre deals (trades)
std::optional<std::vector<MEXCTrade>> parseAggreDeals(const std::string& data) {
    try {
        auto fields = ProtobufParser::parse(data);
        std::vector<MEXCTrade> trades;
        
        for (const auto& field : fields) {
            if (field.tag == 1 && field.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                // This is a deals array element
                auto dealFields = ProtobufParser::parse(field.data);
                MEXCTrade trade = {};
                
                for (const auto& df : dealFields) {
                    switch (df.tag) {
                        case 1: // price
                            if (df.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                                trade.price = std::stod(ProtobufParser::parseString(df.data));
                            }
                            break;
                        case 2: // quantity
                            if (df.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                                trade.quantity = std::stod(ProtobufParser::parseString(df.data));
                            }
                            break;
                        case 3: // trade type
                            if (df.wire_type == ProtobufParser::VARINT) {
                                trade.trade_type = ProtobufParser::parseUint64(df.data);
                            }
                            break;
                        case 4: // timestamp
                            if (df.wire_type == ProtobufParser::VARINT) {
                                trade.timestamp = ProtobufParser::parseUint64(df.data);
                            }
                            break;
                    }
                }
                trades.push_back(trade);
            }
        }
        
        return trades;
    } catch (...) {
        return std::nullopt;
    }
}

// Parse MEXC aggre depth (orderbook) - from field 313
std::optional<MEXCOrderBook> parseAggreDepth(const std::string& data) {
    try {
        auto fields = ProtobufParser::parse(data);
        MEXCOrderBook orderbook = {};
        
        for (const auto& field : fields) {
            if (field.tag == 1 && field.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                // Each ask level
                auto levelFields = ProtobufParser::parse(field.data);
                MEXCOrderBookLevel level = {};
                
                for (const auto& lf : levelFields) {
                    if (lf.tag == 1 && lf.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                        level.price = std::stod(ProtobufParser::parseString(lf.data));
                    } else if (lf.tag == 2 && lf.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                        level.quantity = std::stod(ProtobufParser::parseString(lf.data));
                    }
                }
                if (level.quantity > 0) {  // Skip entries with 0 quantity
                    orderbook.asks.push_back(level);
                }
            }
            else if (field.tag == 2 && field.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                // Each bid level
                auto levelFields = ProtobufParser::parse(field.data);
                MEXCOrderBookLevel level = {};
                
                for (const auto& lf : levelFields) {
                    if (lf.tag == 1 && lf.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                        level.price = std::stod(ProtobufParser::parseString(lf.data));
                    } else if (lf.tag == 2 && lf.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                        level.quantity = std::stod(ProtobufParser::parseString(lf.data));
                    }
                }
                if (level.quantity > 0) {  // Skip entries with 0 quantity
                    orderbook.bids.push_back(level);
                }
            }
            else if (field.tag == 3 && field.wire_type == ProtobufParser::VARINT) {
                // Version
                orderbook.version = ProtobufParser::parseUint64(field.data);
            }
        }
        
        return orderbook;
    } catch (const std::exception& e) {
        return std::nullopt;
    }
}

// Parse MEXC mini ticker
std::optional<MEXCTicker> parseMiniTicker(const std::string& data) {
    try {
        auto fields = ProtobufParser::parse(data);
        MEXCTicker ticker = {};
        
        
        for (const auto& field : fields) {
            switch (field.tag) {
                case 1: // symbol
                    if (field.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                        ticker.symbol = ProtobufParser::parseString(field.data);
                    }
                    break;
                case 2: // price
                    if (field.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                        ticker.price = std::stod(ProtobufParser::parseString(field.data));
                    }
                    break;
                case 3: // volume
                    if (field.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                        ticker.volume = std::stod(ProtobufParser::parseString(field.data));
                    }
                    break;
                case 4: // high
                    if (field.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                        ticker.high = std::stod(ProtobufParser::parseString(field.data));
                    }
                    break;
                case 5: // low
                    if (field.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                        ticker.low = std::stod(ProtobufParser::parseString(field.data));
                    }
                    break;
                case 6: // rate
                    if (field.wire_type == ProtobufParser::LENGTH_DELIMITED) {
                        ticker.rate = std::stod(ProtobufParser::parseString(field.data));
                    }
                    break;
            }
        }
        
        return ticker;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace mexc
}  // namespace arbitrage