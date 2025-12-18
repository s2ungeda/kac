#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <iomanip>

// Simple protobuf varint decoder
uint64_t decodeVarint(const uint8_t* data, size_t& offset, size_t max_len) {
    uint64_t result = 0;
    int shift = 0;
    
    while (offset < max_len) {
        uint8_t byte = data[offset++];
        result |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return result;
        }
        shift += 7;
    }
    return 0;
}

void analyzeProtobuf(const std::string& data) {
    std::cout << "\n=== Analyzing Protobuf Message (length: " << data.length() << ") ===\n";
    
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    size_t offset = 0;
    size_t len = data.length();
    
    // Show first 100 bytes as hex
    std::cout << "First bytes (hex): ";
    for (size_t i = 0; i < std::min(len, size_t(100)); i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[i] << " ";
        if ((i + 1) % 16 == 0) std::cout << "\n                   ";
    }
    std::cout << std::dec << "\n\n";
    
    // Parse fields
    while (offset < len) {
        size_t field_start = offset;
        
        // Parse tag
        uint64_t tag_wire = decodeVarint(bytes, offset, len);
        if (offset >= len) break;
        
        uint32_t tag = tag_wire >> 3;
        uint32_t wire_type = tag_wire & 0x07;
        
        std::cout << "Field " << tag << " (wire type " << wire_type << "): ";
        
        // Parse value based on wire type
        switch (wire_type) {
            case 0: { // VARINT
                uint64_t value = decodeVarint(bytes, offset, len);
                std::cout << "varint = " << value << "\n";
                break;
            }
            case 1: { // FIXED64
                if (offset + 8 > len) break;
                double value;
                std::memcpy(&value, &bytes[offset], 8);
                offset += 8;
                std::cout << "fixed64 = " << value << "\n";
                break;
            }
            case 2: { // LENGTH_DELIMITED
                uint64_t length = decodeVarint(bytes, offset, len);
                std::cout << "length_delimited (len=" << length << ") = ";
                
                if (offset + length <= len && length < 100) {
                    // Try to print as string
                    std::string str(reinterpret_cast<const char*>(&bytes[offset]), length);
                    bool is_printable = true;
                    for (char c : str) {
                        if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
                            is_printable = false;
                            break;
                        }
                    }
                    
                    if (is_printable) {
                        std::cout << "\"" << str << "\"\n";
                    } else {
                        // Show as hex
                        for (size_t i = 0; i < std::min(length, uint64_t(20)); i++) {
                            std::cout << std::hex << std::setw(2) << std::setfill('0') 
                                     << (int)bytes[offset + i] << " ";
                        }
                        if (length > 20) std::cout << "...";
                        std::cout << std::dec << "\n";
                        
                        // If it might be a nested protobuf, recurse
                        if (length > 2 && length < 1000) {
                            std::cout << "  [Nested message:]\n";
                            analyzeProtobuf(std::string(reinterpret_cast<const char*>(&bytes[offset]), length));
                        }
                    }
                } else {
                    std::cout << "[too long]\n";
                }
                
                offset += length;
                break;
            }
            case 5: { // FIXED32
                if (offset + 4 > len) break;
                float value;
                std::memcpy(&value, &bytes[offset], 4);
                offset += 4;
                std::cout << "fixed32 = " << value << "\n";
                break;
            }
            default:
                std::cout << "unknown wire type\n";
                return;
        }
    }
}

int main() {
    // Test with sample MEXC protobuf data
    // This would normally come from the WebSocket
    
    // Example: Simple test message
    std::string test_msg;
    
    // Field 1 (tag=1, type=2): "spot@public.miniTicker.v3.api.pb@BTCUSDT@UTC+8"
    test_msg += "\x0a";  // tag 1, wire type 2
    std::string channel = "spot@public.miniTicker.v3.api.pb@BTCUSDT@UTC+8";
    test_msg += char(channel.length());
    test_msg += channel;
    
    // Field 2 (tag=2, type=2): nested message
    test_msg += "\x12";  // tag 2, wire type 2
    
    // Build nested ticker message
    std::string ticker_msg;
    // Field 1: symbol = "BTCUSDT"
    ticker_msg += "\x0a";  // tag 1, wire type 2
    ticker_msg += "\x07";  // length 7
    ticker_msg += "BTCUSDT";
    // Field 2: price = "102345.67"
    ticker_msg += "\x12\x09";
    ticker_msg += "102345.67";
    
    test_msg += char(ticker_msg.length());
    test_msg += ticker_msg;
    
    analyzeProtobuf(test_msg);
    
    return 0;
}