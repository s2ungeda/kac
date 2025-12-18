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

void analyzeProtobuf(const std::string& data, int depth = 0) {
    std::string indent(depth * 2, ' ');
    
    if (depth == 0) {
        std::cout << "\n=== Analyzing Protobuf Message (length: " << data.length() << ") ===\n";
    }
    
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    size_t offset = 0;
    size_t len = data.length();
    
    if (depth == 0 && len > 50) {
        // Show first 50 bytes as hex
        std::cout << "First bytes (hex): ";
        for (size_t i = 0; i < std::min(len, size_t(50)); i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[i] << " ";
            if ((i + 1) % 16 == 0) std::cout << "\n                   ";
        }
        std::cout << std::dec << "\n\n";
    }
    
    // Parse fields
    while (offset < len) {
        size_t field_start = offset;
        
        // Parse tag
        uint64_t tag_wire = decodeVarint(bytes, offset, len);
        if (offset >= len) break;
        
        uint32_t tag = tag_wire >> 3;
        uint32_t wire_type = tag_wire & 0x07;
        
        std::cout << indent << "Field " << tag << " (wire type " << wire_type << "): ";
        
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
                
                if (offset + length <= len) {
                    // Try to print as string
                    std::string str(reinterpret_cast<const char*>(&bytes[offset]), 
                                   std::min(length, uint64_t(100)));
                    bool is_printable = true;
                    for (char c : str) {
                        if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
                            is_printable = false;
                            break;
                        }
                    }
                    
                    if (is_printable && length < 100) {
                        std::cout << "\"" << str << "\"\n";
                    } else {
                        // Show as hex
                        std::cout << "[hex] ";
                        for (size_t i = 0; i < std::min(length, uint64_t(20)); i++) {
                            std::cout << std::hex << std::setw(2) << std::setfill('0') 
                                     << (int)bytes[offset + i] << " ";
                        }
                        if (length > 20) std::cout << "...";
                        std::cout << std::dec << "\n";
                        
                        // If it might be a nested protobuf, recurse
                        if (length > 2 && depth < 2) {
                            // Special handling for field 313 (order book data)
                            if (tag == 313 && depth == 0) {
                                std::cout << indent << "  [Analyzing OrderBook data:]\n";
                                analyzeProtobuf(std::string(reinterpret_cast<const char*>(&bytes[offset]), length), depth + 1);
                            } else if (length < 500) {
                                std::cout << indent << "  [Analyzing as nested message:]\n";
                                analyzeProtobuf(std::string(reinterpret_cast<const char*>(&bytes[offset]), length), depth + 1);
                            }
                        }
                    }
                } else {
                    std::cout << "[invalid length]\n";
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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <binary_file>\n";
        return 1;
    }
    
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file: " << argv[1] << "\n";
        return 1;
    }
    
    // Read entire file
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::string data(size, '\0');
    file.read(&data[0], size);
    file.close();
    
    std::cout << "Analyzing file: " << argv[1] << " (size: " << size << " bytes)\n";
    analyzeProtobuf(data);
    
    return 0;
}