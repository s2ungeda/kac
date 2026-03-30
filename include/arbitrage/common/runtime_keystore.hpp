#pragma once

/**
 * Runtime Key Store
 *
 * API 키를 메모리 내 암호화 상태로 보관.
 * API 호출 시에만 순간 복호화 → 사용 → 즉시 wipe.
 *
 * 보안 조치:
 *   - AES-256-GCM으로 메모리 내 암호화 (SecretsManager 활용)
 *   - 런타임 랜덤 마스터 키 (프로세스마다 다름)
 *   - mlock(): 마스터 키 페이지 swap 방지
 *   - prctl(PR_SET_DUMPABLE, 0): core dump 비활성화
 *   - with_key() RAII 패턴: 복호화된 평문의 생존 시간 최소화
 */

#include "arbitrage/common/secrets.hpp"
#include <string>
#include <functional>
#include <memory>

namespace arbitrage {

class RuntimeKeyStore {
public:
    RuntimeKeyStore();
    ~RuntimeKeyStore();

    RuntimeKeyStore(const RuntimeKeyStore&) = delete;
    RuntimeKeyStore& operator=(const RuntimeKeyStore&) = delete;

    /**
     * 키 저장 (평문 → 암호화 → 저장, 평문은 wipe)
     * @param name 키 이름 (예: "upbit/main/api_key")
     * @param plaintext 평문 값 — 저장 후 호출자가 wipe해야 함
     */
    void store(const std::string& name, const std::string& plaintext);

    /**
     * 키를 순간 복호화하여 콜백에 전달, 콜백 완료 후 즉시 wipe
     * @param name 키 이름
     * @param func 복호화된 평문을 받는 콜백
     * @return 콜백의 반환값, 키가 없으면 false
     *
     * 사용법:
     *   keystore.with_key("upbit/main/api_key", [&](const std::string& key) {
     *       req.headers["Authorization"] = key;
     *   });
     */
    bool with_key(const std::string& name,
                  const std::function<void(const std::string&)>& func);

    /**
     * 키를 순간 복호화하여 두 키를 동시에 콜백에 전달 (api_key + secret 쌍)
     */
    bool with_key_pair(const std::string& key_name,
                       const std::string& secret_name,
                       const std::function<void(const std::string&, const std::string&)>& func);

    /**
     * 키 존재 여부
     */
    bool contains(const std::string& name) const;

    /**
     * 키 삭제
     */
    void remove(const std::string& name);

    /**
     * 저장된 키 수
     */
    size_t count() const;

    /**
     * 프로세스 보안 설정 (core dump 비활성화 등)
     * main()에서 한 번 호출
     */
    static void harden_process();

private:
    std::unique_ptr<SecretsManager> sm_;
};

// 글로벌 인스턴스
RuntimeKeyStore& runtime_keystore();

}  // namespace arbitrage
