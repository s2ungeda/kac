# TASK 20: 시크릿 관리 (C++)

## 🎯 목표
API 키 등 민감 정보 암호화 저장

---

## 📁 생성할 파일

```
include/arbitrage/common/
└── secrets.hpp
src/common/
└── secrets.cpp
```

---

## 📝 핵심 구현

```cpp
class SecretsManager {
public:
    SecretsManager(const std::string& master_password);
    
    // 저장/조회
    Result<void> store(const std::string& key, const std::string& value);
    Result<std::string> retrieve(const std::string& key);
    Result<void> remove(const std::string& key);
    
    // 파일 저장/로드
    Result<void> save_to_file(const std::string& path);
    Result<void> load_from_file(const std::string& path);
    
private:
    // AES-256-GCM 암호화
    Result<std::string> encrypt(const std::string& plaintext);
    Result<std::string> decrypt(const std::string& ciphertext);
    
    // PBKDF2로 키 유도
    std::array<uint8_t, 32> derive_key(const std::string& password);
    
    std::array<uint8_t, 32> master_key_;
    std::map<std::string, std::string> secrets_;
};
```

---

## ✅ 완료 조건

```
□ AES-256-GCM 암호화
□ PBKDF2 키 유도
□ 파일 저장/로드
□ 메모리 보호
```

---

## 📎 다음 태스크

완료 후: TASK_22_symbol_master.md
