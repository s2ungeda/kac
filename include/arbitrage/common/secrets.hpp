#pragma once

/**
 * Secrets Manager (TASK_16)
 *
 * API 키 등 민감 정보 암호화 저장
 * - AES-256-GCM 암호화
 * - PBKDF2 키 유도
 * - 파일 저장/로드
 * - 메모리 보호
 */

#include "arbitrage/common/error.hpp"

#include <string>
#include <array>
#include <map>
#include <vector>
#include "arbitrage/common/spin_wait.hpp"

#include <cstdint>
#include <chrono>
#include <optional>

namespace arbitrage {

// =============================================================================
// 상수
// =============================================================================
constexpr size_t AES_KEY_SIZE = 32;        // AES-256
constexpr size_t AES_GCM_IV_SIZE = 12;     // GCM IV size
constexpr size_t AES_GCM_TAG_SIZE = 16;    // GCM auth tag size
constexpr size_t PBKDF2_SALT_SIZE = 16;    // Salt size
constexpr int PBKDF2_ITERATIONS = 100000;  // OWASP 권장값

// =============================================================================
// 암호화된 데이터 구조체
// =============================================================================
struct EncryptedData {
    std::vector<uint8_t> ciphertext;
    std::array<uint8_t, AES_GCM_IV_SIZE> iv;
    std::array<uint8_t, AES_GCM_TAG_SIZE> tag;

    // 직렬화
    std::string to_base64() const;
    static Result<EncryptedData> from_base64(const std::string& encoded);
};

// =============================================================================
// 시크릿 메타데이터
// =============================================================================
struct SecretMetadata {
    std::string key_name;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    size_t access_count{0};
    std::string description;
};

// =============================================================================
// 시크릿 매니저
// =============================================================================
class SecretsManager {
public:
    /**
     * 생성자
     * @param master_password 마스터 비밀번호 (키 유도에 사용)
     */
    explicit SecretsManager(const std::string& master_password);

    /**
     * 소멸자 - 메모리 안전하게 정리
     */
    ~SecretsManager();

    // 복사/이동 금지 (보안상)
    SecretsManager(const SecretsManager&) = delete;
    SecretsManager& operator=(const SecretsManager&) = delete;
    SecretsManager(SecretsManager&&) = delete;
    SecretsManager& operator=(SecretsManager&&) = delete;

    // =========================================================================
    // 시크릿 저장/조회
    // =========================================================================

    /**
     * 시크릿 저장
     * @param key 키 이름
     * @param value 저장할 값
     * @param description 설명 (선택)
     * @return 성공 여부
     */
    Result<void> store(
        const std::string& key,
        const std::string& value,
        const std::string& description = ""
    );

    /**
     * 시크릿 조회
     * @param key 키 이름
     * @return 복호화된 값
     */
    Result<std::string> retrieve(const std::string& key);

    /**
     * 시크릿 삭제
     * @param key 키 이름
     * @return 성공 여부
     */
    Result<void> remove(const std::string& key);

    /**
     * 시크릿 존재 여부 확인
     */
    bool contains(const std::string& key) const;

    /**
     * 모든 키 목록 조회
     */
    std::vector<std::string> list_keys() const;

    /**
     * 시크릿 메타데이터 조회
     */
    std::optional<SecretMetadata> get_metadata(const std::string& key) const;

    // =========================================================================
    // 파일 저장/로드
    // =========================================================================

    /**
     * 파일로 저장
     * @param path 파일 경로
     * @return 성공 여부
     */
    Result<void> save_to_file(const std::string& path);

    /**
     * 파일에서 로드
     * @param path 파일 경로
     * @return 성공 여부
     */
    Result<void> load_from_file(const std::string& path);

    // =========================================================================
    // 마스터 비밀번호 관리
    // =========================================================================

    /**
     * 마스터 비밀번호 변경
     * @param old_password 현재 비밀번호
     * @param new_password 새 비밀번호
     * @return 성공 여부
     */
    Result<void> change_master_password(
        const std::string& old_password,
        const std::string& new_password
    );

    /**
     * 마스터 비밀번호 검증
     */
    bool verify_password(const std::string& password) const;

    // =========================================================================
    // 유틸리티
    // =========================================================================

    /**
     * 시크릿 개수
     */
    size_t count() const;

    /**
     * 모든 시크릿 삭제
     */
    void clear();

    // =========================================================================
    // 암호화 유틸리티 (정적 함수)
    // =========================================================================

    /**
     * AES-256-GCM 암호화
     * @param key 암호화 키 (32바이트)
     * @param plaintext 평문
     * @return 암호화된 데이터
     */
    static Result<EncryptedData> encrypt_aes_gcm(
        const std::array<uint8_t, AES_KEY_SIZE>& key,
        const std::string& plaintext
    );

    /**
     * AES-256-GCM 복호화
     * @param key 암호화 키 (32바이트)
     * @param data 암호화된 데이터
     * @return 복호화된 평문
     */
    static Result<std::string> decrypt_aes_gcm(
        const std::array<uint8_t, AES_KEY_SIZE>& key,
        const EncryptedData& data
    );

    /**
     * PBKDF2로 키 유도
     * @param password 비밀번호
     * @param salt 솔트 (16바이트)
     * @param iterations 반복 횟수
     * @return 유도된 키 (32바이트)
     */
    static std::array<uint8_t, AES_KEY_SIZE> derive_key(
        const std::string& password,
        const std::array<uint8_t, PBKDF2_SALT_SIZE>& salt,
        int iterations = PBKDF2_ITERATIONS
    );

    /**
     * 랜덤 바이트 생성
     */
    static void generate_random_bytes(uint8_t* buffer, size_t size);

private:
    // 내부 암호화/복호화 (마스터 키 사용)
    Result<std::string> encrypt(const std::string& plaintext);
    Result<std::string> decrypt(const std::string& ciphertext);

    // 메모리 안전하게 지우기
    static void secure_zero(void* ptr, size_t size);

    // 시크릿 데이터 구조
    struct SecretEntry {
        std::string encrypted_value;  // Base64 인코딩된 암호화 데이터
        SecretMetadata metadata;
    };

    // 멤버 변수
    mutable RWSpinLock mutex_;
    std::array<uint8_t, AES_KEY_SIZE> master_key_;
    std::array<uint8_t, PBKDF2_SALT_SIZE> salt_;
    std::map<std::string, SecretEntry> secrets_;

    // 파일 포맷 버전
    static constexpr uint32_t FILE_VERSION = 1;
    static constexpr char FILE_MAGIC[] = "ARBS";  // ARBitrage Secrets
};

// =============================================================================
// 글로벌 인스턴스 접근자
// =============================================================================

/**
 * 글로벌 SecretsManager 초기화
 * @param master_password 마스터 비밀번호
 */
void init_secrets_manager(const std::string& master_password);

/**
 * 글로벌 SecretsManager 접근
 * @return SecretsManager 참조
 * @throws std::runtime_error 초기화되지 않은 경우
 */
SecretsManager& secrets_manager();

/**
 * 글로벌 SecretsManager 초기화 여부
 */
bool is_secrets_manager_initialized();

// =============================================================================
// 보안 문자열 유틸리티
// =============================================================================

/**
 * 보안 문자열 - 소멸 시 자동으로 메모리 정리
 */
class SecureString {
public:
    SecureString() = default;
    explicit SecureString(const std::string& str);
    explicit SecureString(std::string&& str);
    ~SecureString();

    SecureString(const SecureString& other);
    SecureString& operator=(const SecureString& other);
    SecureString(SecureString&& other) noexcept;
    SecureString& operator=(SecureString&& other) noexcept;

    const std::string& get() const { return data_; }
    const char* c_str() const { return data_.c_str(); }
    size_t size() const { return data_.size(); }
    bool empty() const { return data_.empty(); }

    void clear();

private:
    std::string data_;
};

}  // namespace arbitrage
