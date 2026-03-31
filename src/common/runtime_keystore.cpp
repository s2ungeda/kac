#include "arbitrage/common/runtime_keystore.hpp"
#include "arbitrage/common/logger.hpp"
#include <random>
#include <cstring>

#ifdef __linux__
#include <sys/mman.h>    // mlock
#include <sys/prctl.h>   // prctl
#include <sys/resource.h> // setrlimit
#endif

namespace arbitrage {

// =============================================================================
// 랜덤 마스터 비밀번호 생성 (32바이트 → base64)
// =============================================================================
static std::string generate_runtime_password() {
    std::array<uint8_t, 32> buf;
    SecretsManager::generate_random_bytes(buf.data(), buf.size());

    // hex 인코딩
    static const char hex[] = "0123456789abcdef";
    std::string password;
    password.reserve(64);
    for (auto b : buf) {
        password.push_back(hex[b >> 4]);
        password.push_back(hex[b & 0x0f]);
    }

    // 원본 버퍼 wipe
    std::memset(buf.data(), 0, buf.size());
    return password;
}

// =============================================================================
// 메모리 wipe 헬퍼
// =============================================================================
static void secure_wipe(std::string& s) {
    if (!s.empty()) {
        volatile char* p = const_cast<volatile char*>(s.data());
        for (size_t i = 0; i < s.size(); ++i) {
            p[i] = 0;
        }
        s.clear();
    }
}

// =============================================================================
// RuntimeKeyStore
// =============================================================================

RuntimeKeyStore::RuntimeKeyStore() {
    // 매 프로세스 기동 시 새로운 랜덤 마스터 비밀번호
    std::string password = generate_runtime_password();
    sm_ = std::make_unique<SecretsManager>(password);

    // 비밀번호 즉시 wipe
    secure_wipe(password);

    auto logger = Logger::get("keystore");
    if (logger) {
        logger->info("[RuntimeKeyStore] Initialized with ephemeral master key");
    }
}

RuntimeKeyStore::~RuntimeKeyStore() = default;

void RuntimeKeyStore::store(const std::string& name, const std::string& plaintext) {
    auto result = sm_->store(name, plaintext);
    if (result.has_error()) {
        auto logger = Logger::get("keystore");
        if (logger) {
            logger->error("[RuntimeKeyStore] Failed to store key: {}", name);
        }
    }
}

bool RuntimeKeyStore::with_key(
    const std::string& name,
    const std::function<void(const std::string&)>& func)
{
    auto result = sm_->retrieve(name);
    if (result.has_error()) {
        return false;
    }

    std::string plaintext = std::move(result.value());
    func(plaintext);

    // 즉시 wipe
    secure_wipe(plaintext);
    return true;
}

bool RuntimeKeyStore::with_key_pair(
    const std::string& key_name,
    const std::string& secret_name,
    const std::function<void(const std::string&, const std::string&)>& func)
{
    auto key_result = sm_->retrieve(key_name);
    auto secret_result = sm_->retrieve(secret_name);

    if (key_result.has_error() || secret_result.has_error()) {
        return false;
    }

    std::string key = std::move(key_result.value());
    std::string secret = std::move(secret_result.value());

    func(key, secret);

    // 즉시 wipe
    secure_wipe(key);
    secure_wipe(secret);
    return true;
}

bool RuntimeKeyStore::contains(const std::string& name) const {
    return sm_->contains(name);
}

void RuntimeKeyStore::remove(const std::string& name) {
    sm_->remove(name);
}

size_t RuntimeKeyStore::count() const {
    return sm_->count();
}

// =============================================================================
// 프로세스 보안 강화
// =============================================================================
void RuntimeKeyStore::harden_process() {
#ifdef __linux__
    // core dump 비활성화
    prctl(PR_SET_DUMPABLE, 0);

    // RLIMIT_CORE = 0
    struct rlimit rl = {0, 0};
    setrlimit(RLIMIT_CORE, &rl);

    auto logger = Logger::get("keystore");
    if (logger) {
        logger->info("[RuntimeKeyStore] Process hardened: core dump disabled");
    }
#endif
}

// =============================================================================
// 글로벌 인스턴스
// =============================================================================
namespace { RuntimeKeyStore* g_set_runtime_keystore_override = nullptr; }
RuntimeKeyStore& runtime_keystore() {
    if (g_set_runtime_keystore_override) return *g_set_runtime_keystore_override;
    static RuntimeKeyStore instance;
    return instance;
}
void set_runtime_keystore(RuntimeKeyStore* p) { g_set_runtime_keystore_override = p; }

}  // namespace arbitrage
