#include "arbitrage/common/crypto.hpp"
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <algorithm>
#include <cstring>

namespace arbitrage {

std::string hmac_sha256(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len;
    
    HMAC(EVP_sha256(), key.c_str(), key.length(),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         result, &result_len);
    
    std::string hmac_result(reinterpret_cast<char*>(result), result_len);
    return hmac_result;
}

std::string hmac_sha512(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len;
    
    HMAC(EVP_sha512(), key.c_str(), key.length(),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         result, &result_len);
    
    std::string hmac_result(reinterpret_cast<char*>(result), result_len);
    return hmac_result;
}

std::string sha256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash);
    return std::string(reinterpret_cast<char*>(hash), SHA256_DIGEST_LENGTH);
}

std::string sha512(const std::string& data) {
    unsigned char hash[SHA512_DIGEST_LENGTH];
    SHA512(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash);
    return std::string(reinterpret_cast<char*>(hash), SHA512_DIGEST_LENGTH);
}

std::string base64_encode(const std::string& data) {
    BIO *bio, *b64;
    BUF_MEM *bufferPtr;
    
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, data.c_str(), data.length());
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);
    
    std::string result(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);
    
    return result;
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    return base64_encode(std::string(data.begin(), data.end()));
}

std::string base64_decode(const std::string& encoded) {
    BIO *bio, *b64;
    char* buffer = static_cast<char*>(malloc(encoded.length()));
    memset(buffer, 0, encoded.length());
    
    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new_mem_buf(encoded.c_str(), -1);
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    int decoded_length = BIO_read(bio, buffer, encoded.length());
    BIO_free_all(bio);
    
    std::string result(buffer, decoded_length);
    free(buffer);
    
    return result;
}

std::string base64url_encode(const std::string& data) {
    std::string encoded = base64_encode(data);
    
    // URL-safe: + -> -, / -> _, remove padding
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    encoded.erase(std::remove(encoded.begin(), encoded.end(), '='), encoded.end());
    
    return encoded;
}

std::string base64url_decode(const std::string& encoded) {
    return base64_decode(encoded);
}

std::string generate_uuid() {
    // 간단한 UUID 생성
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    
    std::ostringstream oss;
    for (int i = 0; i < 8; i++) oss << std::hex << dis(gen);
    oss << "-";
    for (int i = 0; i < 4; i++) oss << std::hex << dis(gen);
    oss << "-4"; // version 4
    for (int i = 0; i < 3; i++) oss << std::hex << dis(gen);
    oss << "-";
    oss << std::hex << ((dis(gen) & 0x3) | 0x8); // variant
    for (int i = 0; i < 3; i++) oss << std::hex << dis(gen);
    oss << "-";
    for (int i = 0; i < 12; i++) oss << std::hex << dis(gen);
    
    return oss.str();
}

int64_t get_timestamp_ms() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

int64_t get_timestamp_us() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

std::string to_hex(const std::string& data) {
    std::ostringstream oss;
    for (unsigned char c : data) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return oss.str();
}

std::string to_hex(const std::vector<uint8_t>& data) {
    return to_hex(std::string(data.begin(), data.end()));
}

}  // namespace arbitrage