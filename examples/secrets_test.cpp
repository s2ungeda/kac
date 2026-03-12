/**
 * Secrets Manager Test (TASK_16)
 *
 * API 키 암호화 저장 테스트
 * - AES-256-GCM 암호화/복호화
 * - PBKDF2 키 유도
 * - 파일 저장/로드
 * - 비밀번호 변경
 */

#include "arbitrage/common/secrets.hpp"

#include <iostream>
#include <cmath>
#include <cstring>
#include <filesystem>

using namespace arbitrage;

// 테스트 결과 카운터
static int tests_passed = 0;
static int tests_failed = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::cout << "[FAIL] " << msg << "\n";
        ++tests_failed;
    } else {
        std::cout << "[PASS] " << msg << "\n";
        ++tests_passed;
    }
}

// =============================================================================
// Test: Basic Store and Retrieve
// =============================================================================
void test_store_retrieve() {
    std::cout << "\n=== Test: store_retrieve ===\n";

    SecretsManager sm("test_master_password");

    // 시크릿 저장
    auto store_result = sm.store("api_key", "my_secret_api_key_12345");
    check(store_result.has_value(), "Store secret should succeed");

    // 시크릿 조회
    auto retrieve_result = sm.retrieve("api_key");
    check(retrieve_result.has_value(), "Retrieve secret should succeed");
    check(retrieve_result.value() == "my_secret_api_key_12345", "Retrieved value matches original");

    // 존재하지 않는 키 조회
    auto not_found = sm.retrieve("nonexistent_key");
    check(not_found.has_error(), "Retrieve nonexistent key should fail");
}

// =============================================================================
// Test: Multiple Secrets
// =============================================================================
void test_multiple_secrets() {
    std::cout << "\n=== Test: multiple_secrets ===\n";

    SecretsManager sm("password123");

    sm.store("upbit_key", "upbit_api_key_value");
    sm.store("upbit_secret", "upbit_secret_value");
    sm.store("binance_key", "binance_api_key_value");
    sm.store("binance_secret", "binance_secret_value");

    check(sm.count() == 4, "Should have 4 secrets");

    auto upbit = sm.retrieve("upbit_key");
    auto binance = sm.retrieve("binance_key");

    check(upbit.value() == "upbit_api_key_value", "Upbit key matches");
    check(binance.value() == "binance_api_key_value", "Binance key matches");

    // 키 목록 조회
    auto keys = sm.list_keys();
    check(keys.size() == 4, "list_keys returns 4 keys");
}

// =============================================================================
// Test: Remove Secret
// =============================================================================
void test_remove_secret() {
    std::cout << "\n=== Test: remove_secret ===\n";

    SecretsManager sm("password");

    sm.store("key1", "value1");
    sm.store("key2", "value2");
    check(sm.count() == 2, "Initially 2 secrets");

    auto remove_result = sm.remove("key1");
    check(remove_result.has_value(), "Remove should succeed");
    check(sm.count() == 1, "After remove: 1 secret");
    check(!sm.contains("key1"), "key1 no longer exists");
    check(sm.contains("key2"), "key2 still exists");
}

// =============================================================================
// Test: Update Secret
// =============================================================================
void test_update_secret() {
    std::cout << "\n=== Test: update_secret ===\n";

    SecretsManager sm("password");

    sm.store("api_key", "original_value");
    auto v1 = sm.retrieve("api_key");
    check(v1.value() == "original_value", "Original value");

    sm.store("api_key", "updated_value");
    auto v2 = sm.retrieve("api_key");
    check(v2.value() == "updated_value", "Updated value");

    check(sm.count() == 1, "Still only 1 secret (updated, not added)");
}

// =============================================================================
// Test: Password Verification
// =============================================================================
void test_password_verification() {
    std::cout << "\n=== Test: password_verification ===\n";

    SecretsManager sm("correct_password");

    check(sm.verify_password("correct_password"), "Correct password verifies");
    check(!sm.verify_password("wrong_password"), "Wrong password fails");
    check(!sm.verify_password(""), "Empty password fails");
}

// =============================================================================
// Test: AES-GCM Encryption (Static)
// =============================================================================
void test_aes_gcm_encryption() {
    std::cout << "\n=== Test: aes_gcm_encryption ===\n";

    // 테스트 키 생성
    std::array<uint8_t, PBKDF2_SALT_SIZE> salt;
    SecretsManager::generate_random_bytes(salt.data(), salt.size());
    auto key = SecretsManager::derive_key("test_password", salt);

    std::string plaintext = "Hello, this is a secret message!";

    // 암호화
    auto encrypt_result = SecretsManager::encrypt_aes_gcm(key, plaintext);
    check(encrypt_result.has_value(), "Encryption succeeds");

    // 복호화
    auto decrypt_result = SecretsManager::decrypt_aes_gcm(key, encrypt_result.value());
    check(decrypt_result.has_value(), "Decryption succeeds");
    check(decrypt_result.value() == plaintext, "Decrypted matches original");

    // 다른 키로 복호화 시도
    std::array<uint8_t, PBKDF2_SALT_SIZE> salt2;
    SecretsManager::generate_random_bytes(salt2.data(), salt2.size());
    auto wrong_key = SecretsManager::derive_key("wrong_password", salt2);

    auto wrong_decrypt = SecretsManager::decrypt_aes_gcm(wrong_key, encrypt_result.value());
    check(wrong_decrypt.has_error(), "Decryption with wrong key fails");
}

// =============================================================================
// Test: PBKDF2 Key Derivation
// =============================================================================
void test_pbkdf2_derivation() {
    std::cout << "\n=== Test: pbkdf2_derivation ===\n";

    std::array<uint8_t, PBKDF2_SALT_SIZE> salt;
    SecretsManager::generate_random_bytes(salt.data(), salt.size());

    auto key1 = SecretsManager::derive_key("password", salt);
    auto key2 = SecretsManager::derive_key("password", salt);
    auto key3 = SecretsManager::derive_key("different", salt);

    // 같은 비밀번호 + 솔트 = 같은 키
    check(key1 == key2, "Same password + salt = same key");

    // 다른 비밀번호 = 다른 키
    check(key1 != key3, "Different password = different key");

    // 키 크기 확인
    check(key1.size() == AES_KEY_SIZE, "Key size is 32 bytes (AES-256)");
}

// =============================================================================
// Test: File Save and Load
// =============================================================================
void test_file_save_load() {
    std::cout << "\n=== Test: file_save_load ===\n";

    const std::string test_file = "/tmp/secrets_test.dat";

    // 저장
    {
        SecretsManager sm("file_test_password");
        sm.store("key1", "value1");
        sm.store("key2", "value2");
        sm.store("key3", "value3");

        auto save_result = sm.save_to_file(test_file);
        check(save_result.has_value(), "Save to file succeeds");
    }

    // 로드
    {
        SecretsManager sm("file_test_password");
        auto load_result = sm.load_from_file(test_file);
        check(load_result.has_value(), "Load from file succeeds");
        check(sm.count() == 3, "Loaded 3 secrets");

        // 값 확인 (같은 비밀번호이므로 복호화 가능)
        // Note: 로드 시 솔트가 파일에서 읽히므로 복호화 가능
    }

    // 파일 정리
    std::filesystem::remove(test_file);
    check(!std::filesystem::exists(test_file), "Test file cleaned up");
}

// =============================================================================
// Test: SecureString
// =============================================================================
void test_secure_string() {
    std::cout << "\n=== Test: secure_string ===\n";

    {
        SecureString ss("sensitive_data");
        check(ss.get() == "sensitive_data", "SecureString stores data");
        check(ss.size() == 14, "SecureString size correct");
        check(!ss.empty(), "SecureString not empty");
    }
    // 소멸자에서 자동으로 메모리 정리됨

    SecureString ss2;
    check(ss2.empty(), "Default SecureString is empty");

    ss2 = SecureString("new_data");
    check(ss2.get() == "new_data", "SecureString assignment works");

    ss2.clear();
    check(ss2.empty(), "SecureString clear works");
}

// =============================================================================
// Test: Metadata
// =============================================================================
void test_metadata() {
    std::cout << "\n=== Test: metadata ===\n";

    SecretsManager sm("password");
    sm.store("api_key", "secret_value", "My API key for testing");

    auto meta = sm.get_metadata("api_key");
    check(meta.has_value(), "Metadata exists");
    check(meta->key_name == "api_key", "Key name matches");
    check(meta->description == "My API key for testing", "Description matches");
    check(meta->access_count == 0, "Initial access count is 0");

    // 조회하면 access_count 증가
    sm.retrieve("api_key");
    meta = sm.get_metadata("api_key");
    check(meta->access_count == 1, "Access count incremented");
}

// =============================================================================
// Test: Empty Key/Value Validation
// =============================================================================
void test_validation() {
    std::cout << "\n=== Test: validation ===\n";

    SecretsManager sm("password");

    auto result1 = sm.store("", "value");
    check(result1.has_error(), "Empty key should fail");

    auto result2 = sm.store("key", "");
    check(result2.has_error(), "Empty value should fail");
}

// =============================================================================
// Main
// =============================================================================
int main() {
    std::cout << "========================================\n";
    std::cout << "  Secrets Manager Test (TASK_16)\n";
    std::cout << "========================================\n";

    test_store_retrieve();
    test_multiple_secrets();
    test_remove_secret();
    test_update_secret();
    test_password_verification();
    test_aes_gcm_encryption();
    test_pbkdf2_derivation();
    test_file_save_load();
    test_secure_string();
    test_metadata();
    test_validation();

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "========================================\n";

    return tests_failed > 0 ? 1 : 0;
}
