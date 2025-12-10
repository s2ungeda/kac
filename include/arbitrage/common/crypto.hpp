#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace arbitrage {

// HMAC 함수들
std::string hmac_sha256(const std::string& key, const std::string& data);
std::string hmac_sha512(const std::string& key, const std::string& data);

// 해시 함수들
std::string sha256(const std::string& data);
std::string sha512(const std::string& data);

// Base64 인코딩/디코딩
std::string base64_encode(const std::string& data);
std::string base64_encode(const std::vector<uint8_t>& data);
std::string base64_decode(const std::string& encoded);

// Base64URL 인코딩 (JWT용)
std::string base64url_encode(const std::string& data);
std::string base64url_decode(const std::string& encoded);

// UUID 생성
std::string generate_uuid();

// 타임스탬프
int64_t get_timestamp_ms();  // milliseconds since epoch
int64_t get_timestamp_us();  // microseconds since epoch

// Hex 인코딩
std::string to_hex(const std::string& data);
std::string to_hex(const std::vector<uint8_t>& data);

}  // namespace arbitrage