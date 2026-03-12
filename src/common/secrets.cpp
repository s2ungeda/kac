/**
 * Secrets Manager Implementation (TASK_16)
 */

#include "arbitrage/common/secrets.hpp"
#include "arbitrage/common/crypto.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/kdf.h>

#include <fstream>
#include <sstream>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <random>

namespace arbitrage {

// =============================================================================
// 글로벌 인스턴스
// =============================================================================
static std::unique_ptr<SecretsManager> g_secrets_manager;
static std::mutex g_secrets_init_mutex;

void init_secrets_manager(const std::string& master_password) {
    std::lock_guard<std::mutex> lock(g_secrets_init_mutex);
    g_secrets_manager = std::make_unique<SecretsManager>(master_password);
}

SecretsManager& secrets_manager() {
    if (!g_secrets_manager) {
        throw std::runtime_error("SecretsManager not initialized. Call init_secrets_manager() first.");
    }
    return *g_secrets_manager;
}

bool is_secrets_manager_initialized() {
    return g_secrets_manager != nullptr;
}

// =============================================================================
// EncryptedData 직렬화
// =============================================================================
std::string EncryptedData::to_base64() const {
    // Format: iv(12) + tag(16) + ciphertext
    std::vector<uint8_t> combined;
    combined.reserve(AES_GCM_IV_SIZE + AES_GCM_TAG_SIZE + ciphertext.size());

    combined.insert(combined.end(), iv.begin(), iv.end());
    combined.insert(combined.end(), tag.begin(), tag.end());
    combined.insert(combined.end(), ciphertext.begin(), ciphertext.end());

    return base64_encode(combined);
}

Result<EncryptedData> EncryptedData::from_base64(const std::string& encoded) {
    std::string decoded = base64_decode(encoded);

    if (decoded.size() < AES_GCM_IV_SIZE + AES_GCM_TAG_SIZE) {
        return Error(ErrorCode::InvalidParameter, "Invalid encrypted data format");
    }

    EncryptedData data;

    // IV
    std::memcpy(data.iv.data(), decoded.data(), AES_GCM_IV_SIZE);

    // Tag
    std::memcpy(data.tag.data(), decoded.data() + AES_GCM_IV_SIZE, AES_GCM_TAG_SIZE);

    // Ciphertext
    size_t ct_size = decoded.size() - AES_GCM_IV_SIZE - AES_GCM_TAG_SIZE;
    data.ciphertext.resize(ct_size);
    std::memcpy(data.ciphertext.data(),
                decoded.data() + AES_GCM_IV_SIZE + AES_GCM_TAG_SIZE,
                ct_size);

    return data;
}

// =============================================================================
// SecretsManager 생성자/소멸자
// =============================================================================
SecretsManager::SecretsManager(const std::string& master_password) {
    // 랜덤 솔트 생성
    generate_random_bytes(salt_.data(), salt_.size());

    // 마스터 키 유도
    master_key_ = derive_key(master_password, salt_);
}

SecretsManager::~SecretsManager() {
    // 메모리 안전하게 정리
    secure_zero(master_key_.data(), master_key_.size());
    secure_zero(salt_.data(), salt_.size());

    for (auto& [key, entry] : secrets_) {
        secure_zero(entry.encrypted_value.data(), entry.encrypted_value.size());
    }
    secrets_.clear();
}

// =============================================================================
// 시크릿 저장/조회
// =============================================================================
Result<void> SecretsManager::store(
    const std::string& key,
    const std::string& value,
    const std::string& description
) {
    if (key.empty()) {
        return Error(ErrorCode::InvalidParameter, "Key cannot be empty");
    }
    if (value.empty()) {
        return Error(ErrorCode::InvalidParameter, "Value cannot be empty");
    }

    // 암호화
    auto encrypted_result = encrypt(value);
    if (!encrypted_result) {
        return encrypted_result.error();
    }

    auto now = std::chrono::system_clock::now();

    std::unique_lock lock(mutex_);

    SecretEntry entry;
    entry.encrypted_value = encrypted_result.value();
    entry.metadata.key_name = key;
    entry.metadata.description = description;
    entry.metadata.access_count = 0;

    auto it = secrets_.find(key);
    if (it != secrets_.end()) {
        // 업데이트
        entry.metadata.created_at = it->second.metadata.created_at;
        entry.metadata.updated_at = now;
    } else {
        // 새로 추가
        entry.metadata.created_at = now;
        entry.metadata.updated_at = now;
    }

    secrets_[key] = std::move(entry);

    return {};
}

Result<std::string> SecretsManager::retrieve(const std::string& key) {
    std::unique_lock lock(mutex_);

    auto it = secrets_.find(key);
    if (it == secrets_.end()) {
        return Error(ErrorCode::NotFound, "Secret not found: " + key);
    }

    // 접근 카운트 증가
    it->second.metadata.access_count++;

    // 복호화
    return decrypt(it->second.encrypted_value);
}

Result<void> SecretsManager::remove(const std::string& key) {
    std::unique_lock lock(mutex_);

    auto it = secrets_.find(key);
    if (it == secrets_.end()) {
        return Error(ErrorCode::NotFound, "Secret not found: " + key);
    }

    // 메모리 안전하게 지우기
    secure_zero(it->second.encrypted_value.data(), it->second.encrypted_value.size());
    secrets_.erase(it);

    return {};
}

bool SecretsManager::contains(const std::string& key) const {
    std::shared_lock lock(mutex_);
    return secrets_.find(key) != secrets_.end();
}

std::vector<std::string> SecretsManager::list_keys() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> keys;
    keys.reserve(secrets_.size());
    for (const auto& [key, _] : secrets_) {
        keys.push_back(key);
    }
    return keys;
}

std::optional<SecretMetadata> SecretsManager::get_metadata(const std::string& key) const {
    std::shared_lock lock(mutex_);
    auto it = secrets_.find(key);
    if (it == secrets_.end()) {
        return std::nullopt;
    }
    return it->second.metadata;
}

// =============================================================================
// 파일 저장/로드
// =============================================================================
Result<void> SecretsManager::save_to_file(const std::string& path) {
    std::shared_lock lock(mutex_);

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return Error(ErrorCode::FileError, "Failed to open file for writing: " + path);
    }

    // 매직 + 버전
    file.write(FILE_MAGIC, 4);
    file.write(reinterpret_cast<const char*>(&FILE_VERSION), sizeof(FILE_VERSION));

    // 솔트
    file.write(reinterpret_cast<const char*>(salt_.data()), salt_.size());

    // 시크릿 개수
    uint32_t count = static_cast<uint32_t>(secrets_.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));

    // 각 시크릿
    for (const auto& [key, entry] : secrets_) {
        // 키 길이 + 키
        uint32_t key_len = static_cast<uint32_t>(key.size());
        file.write(reinterpret_cast<const char*>(&key_len), sizeof(key_len));
        file.write(key.data(), key_len);

        // 암호화된 값 길이 + 값
        uint32_t value_len = static_cast<uint32_t>(entry.encrypted_value.size());
        file.write(reinterpret_cast<const char*>(&value_len), sizeof(value_len));
        file.write(entry.encrypted_value.data(), value_len);

        // 메타데이터: 설명
        uint32_t desc_len = static_cast<uint32_t>(entry.metadata.description.size());
        file.write(reinterpret_cast<const char*>(&desc_len), sizeof(desc_len));
        file.write(entry.metadata.description.data(), desc_len);

        // 메타데이터: 타임스탬프
        auto created = std::chrono::system_clock::to_time_t(entry.metadata.created_at);
        auto updated = std::chrono::system_clock::to_time_t(entry.metadata.updated_at);
        file.write(reinterpret_cast<const char*>(&created), sizeof(created));
        file.write(reinterpret_cast<const char*>(&updated), sizeof(updated));
    }

    if (!file) {
        return Error(ErrorCode::FileError, "Failed to write to file: " + path);
    }

    return {};
}

Result<void> SecretsManager::load_from_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Error(ErrorCode::FileError, "Failed to open file for reading: " + path);
    }

    // 매직 확인
    char magic[4];
    file.read(magic, 4);
    if (std::memcmp(magic, FILE_MAGIC, 4) != 0) {
        return Error(ErrorCode::InvalidParameter, "Invalid file format");
    }

    // 버전 확인
    uint32_t version;
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != FILE_VERSION) {
        return Error(ErrorCode::InvalidParameter, "Unsupported file version");
    }

    // 솔트 읽기 (파일에서 로드하므로 새 솔트 사용)
    std::array<uint8_t, PBKDF2_SALT_SIZE> file_salt;
    file.read(reinterpret_cast<char*>(file_salt.data()), file_salt.size());

    // 시크릿 개수
    uint32_t count;
    file.read(reinterpret_cast<char*>(&count), sizeof(count));

    // 임시 저장
    std::map<std::string, SecretEntry> loaded_secrets;

    for (uint32_t i = 0; i < count; ++i) {
        // 키
        uint32_t key_len;
        file.read(reinterpret_cast<char*>(&key_len), sizeof(key_len));
        std::string key(key_len, '\0');
        file.read(key.data(), key_len);

        // 암호화된 값
        uint32_t value_len;
        file.read(reinterpret_cast<char*>(&value_len), sizeof(value_len));
        std::string encrypted_value(value_len, '\0');
        file.read(encrypted_value.data(), value_len);

        // 설명
        uint32_t desc_len;
        file.read(reinterpret_cast<char*>(&desc_len), sizeof(desc_len));
        std::string description(desc_len, '\0');
        file.read(description.data(), desc_len);

        // 타임스탬프
        time_t created, updated;
        file.read(reinterpret_cast<char*>(&created), sizeof(created));
        file.read(reinterpret_cast<char*>(&updated), sizeof(updated));

        SecretEntry entry;
        entry.encrypted_value = std::move(encrypted_value);
        entry.metadata.key_name = key;
        entry.metadata.description = std::move(description);
        entry.metadata.created_at = std::chrono::system_clock::from_time_t(created);
        entry.metadata.updated_at = std::chrono::system_clock::from_time_t(updated);
        entry.metadata.access_count = 0;

        loaded_secrets[key] = std::move(entry);
    }

    if (!file) {
        return Error(ErrorCode::FileError, "Failed to read from file: " + path);
    }

    // 성공 시 기존 데이터 교체
    std::unique_lock lock(mutex_);
    salt_ = file_salt;
    secrets_ = std::move(loaded_secrets);

    return {};
}

// =============================================================================
// 마스터 비밀번호 관리
// =============================================================================
Result<void> SecretsManager::change_master_password(
    const std::string& old_password,
    const std::string& new_password
) {
    if (!verify_password(old_password)) {
        return Error(ErrorCode::AuthenticationFailed, "Invalid old password");
    }

    std::unique_lock lock(mutex_);

    // 모든 시크릿 복호화
    std::map<std::string, std::string> decrypted;
    for (const auto& [key, entry] : secrets_) {
        auto result = decrypt(entry.encrypted_value);
        if (!result) {
            return result.error();
        }
        decrypted[key] = result.value();
    }

    // 새 솔트 생성
    std::array<uint8_t, PBKDF2_SALT_SIZE> new_salt;
    generate_random_bytes(new_salt.data(), new_salt.size());

    // 새 마스터 키 유도
    auto new_key = derive_key(new_password, new_salt);

    // 이전 키 안전하게 지우기
    secure_zero(master_key_.data(), master_key_.size());
    secure_zero(salt_.data(), salt_.size());

    // 새 키/솔트 적용
    master_key_ = new_key;
    salt_ = new_salt;

    // 모든 시크릿 재암호화
    for (auto& [key, entry] : secrets_) {
        auto encrypted = encrypt(decrypted[key]);
        if (!encrypted) {
            return encrypted.error();
        }
        entry.encrypted_value = encrypted.value();
        entry.metadata.updated_at = std::chrono::system_clock::now();
    }

    // 복호화된 데이터 안전하게 지우기
    for (auto& [key, value] : decrypted) {
        secure_zero(value.data(), value.size());
    }

    return {};
}

bool SecretsManager::verify_password(const std::string& password) const {
    auto test_key = derive_key(password, salt_);
    bool match = std::memcmp(test_key.data(), master_key_.data(), AES_KEY_SIZE) == 0;
    secure_zero(test_key.data(), test_key.size());
    return match;
}

// =============================================================================
// 유틸리티
// =============================================================================
size_t SecretsManager::count() const {
    std::shared_lock lock(mutex_);
    return secrets_.size();
}

void SecretsManager::clear() {
    std::unique_lock lock(mutex_);
    for (auto& [key, entry] : secrets_) {
        secure_zero(entry.encrypted_value.data(), entry.encrypted_value.size());
    }
    secrets_.clear();
}

// =============================================================================
// 암호화 유틸리티 (정적)
// =============================================================================
Result<EncryptedData> SecretsManager::encrypt_aes_gcm(
    const std::array<uint8_t, AES_KEY_SIZE>& key,
    const std::string& plaintext
) {
    EncryptedData result;

    // 랜덤 IV 생성
    generate_random_bytes(result.iv.data(), result.iv.size());

    // 암호화 컨텍스트 생성
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return Error(ErrorCode::CryptoError, "Failed to create cipher context");
    }

    // RAII cleanup
    auto ctx_cleanup = [](EVP_CIPHER_CTX* c) { EVP_CIPHER_CTX_free(c); };
    std::unique_ptr<EVP_CIPHER_CTX, decltype(ctx_cleanup)> ctx_guard(ctx, ctx_cleanup);

    // 암호화 초기화
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), result.iv.data()) != 1) {
        return Error(ErrorCode::CryptoError, "Failed to initialize encryption");
    }

    // 암호화
    result.ciphertext.resize(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int len = 0;
    int ciphertext_len = 0;

    if (EVP_EncryptUpdate(ctx,
                          result.ciphertext.data(), &len,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          plaintext.size()) != 1) {
        return Error(ErrorCode::CryptoError, "Failed to encrypt data");
    }
    ciphertext_len = len;

    if (EVP_EncryptFinal_ex(ctx, result.ciphertext.data() + len, &len) != 1) {
        return Error(ErrorCode::CryptoError, "Failed to finalize encryption");
    }
    ciphertext_len += len;
    result.ciphertext.resize(ciphertext_len);

    // 인증 태그 가져오기
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, AES_GCM_TAG_SIZE, result.tag.data()) != 1) {
        return Error(ErrorCode::CryptoError, "Failed to get auth tag");
    }

    return result;
}

Result<std::string> SecretsManager::decrypt_aes_gcm(
    const std::array<uint8_t, AES_KEY_SIZE>& key,
    const EncryptedData& data
) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return Error(ErrorCode::CryptoError, "Failed to create cipher context");
    }

    auto ctx_cleanup = [](EVP_CIPHER_CTX* c) { EVP_CIPHER_CTX_free(c); };
    std::unique_ptr<EVP_CIPHER_CTX, decltype(ctx_cleanup)> ctx_guard(ctx, ctx_cleanup);

    // 복호화 초기화
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), data.iv.data()) != 1) {
        return Error(ErrorCode::CryptoError, "Failed to initialize decryption");
    }

    // 복호화
    std::vector<uint8_t> plaintext(data.ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
    int len = 0;
    int plaintext_len = 0;

    if (EVP_DecryptUpdate(ctx,
                          plaintext.data(), &len,
                          data.ciphertext.data(),
                          data.ciphertext.size()) != 1) {
        return Error(ErrorCode::CryptoError, "Failed to decrypt data");
    }
    plaintext_len = len;

    // 인증 태그 설정
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, AES_GCM_TAG_SIZE,
                            const_cast<uint8_t*>(data.tag.data())) != 1) {
        return Error(ErrorCode::CryptoError, "Failed to set auth tag");
    }

    // 최종화 및 태그 검증
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        return Error(ErrorCode::CryptoError, "Authentication failed - data may be tampered");
    }
    plaintext_len += len;

    return std::string(reinterpret_cast<char*>(plaintext.data()), plaintext_len);
}

std::array<uint8_t, AES_KEY_SIZE> SecretsManager::derive_key(
    const std::string& password,
    const std::array<uint8_t, PBKDF2_SALT_SIZE>& salt,
    int iterations
) {
    std::array<uint8_t, AES_KEY_SIZE> key;

    if (PKCS5_PBKDF2_HMAC(
            password.c_str(), password.size(),
            salt.data(), salt.size(),
            iterations,
            EVP_sha256(),
            key.size(), key.data()) != 1) {
        // 실패 시 빈 키 반환 (호출자가 처리)
        std::memset(key.data(), 0, key.size());
    }

    return key;
}

void SecretsManager::generate_random_bytes(uint8_t* buffer, size_t size) {
    if (RAND_bytes(buffer, static_cast<int>(size)) != 1) {
        // Fallback to less secure random
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dis(0, 255);
        for (size_t i = 0; i < size; ++i) {
            buffer[i] = static_cast<uint8_t>(dis(gen));
        }
    }
}

// =============================================================================
// 내부 암호화/복호화
// =============================================================================
Result<std::string> SecretsManager::encrypt(const std::string& plaintext) {
    auto result = encrypt_aes_gcm(master_key_, plaintext);
    if (!result) {
        return result.error();
    }
    return result.value().to_base64();
}

Result<std::string> SecretsManager::decrypt(const std::string& ciphertext) {
    auto data_result = EncryptedData::from_base64(ciphertext);
    if (!data_result) {
        return data_result.error();
    }
    return decrypt_aes_gcm(master_key_, data_result.value());
}

// =============================================================================
// 메모리 안전하게 지우기
// =============================================================================
void SecretsManager::secure_zero(void* ptr, size_t size) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (size--) {
        *p++ = 0;
    }
}

// =============================================================================
// SecureString 구현
// =============================================================================
SecureString::SecureString(const std::string& str) : data_(str) {}

SecureString::SecureString(std::string&& str) : data_(std::move(str)) {}

SecureString::~SecureString() {
    clear();
}

SecureString::SecureString(const SecureString& other) : data_(other.data_) {}

SecureString& SecureString::operator=(const SecureString& other) {
    if (this != &other) {
        clear();
        data_ = other.data_;
    }
    return *this;
}

SecureString::SecureString(SecureString&& other) noexcept : data_(std::move(other.data_)) {}

SecureString& SecureString::operator=(SecureString&& other) noexcept {
    if (this != &other) {
        clear();
        data_ = std::move(other.data_);
    }
    return *this;
}

void SecureString::clear() {
    if (!data_.empty()) {
        volatile char* p = &data_[0];
        size_t size = data_.size();
        while (size--) {
            *p++ = 0;
        }
        data_.clear();
    }
}

}  // namespace arbitrage
