# TASK 14: 거래소 간 송금 (C++)

## 🎯 목표
XRP 거래소 간 송금 관리

---

## ⚠️ 주의사항

```
필수:
- Destination Tag 처리 (XRP 필수)
- 송금 상태 추적
- 타임아웃 처리
- 출금 주소 화이트리스트
```

---

## 📁 생성할 파일

```
include/arbitrage/executor/
└── transfer.hpp
src/executor/
└── transfer.cpp
```

---

## 📝 핵심 구현

```cpp
// 송금 요청
struct TransferRequest {
    Exchange from;
    Exchange to;
    std::string coin{"XRP"};
    double amount;
    std::string to_address;
    std::optional<std::string> destination_tag;  // XRP 필수!
};

// 송금 상태
enum class TransferStatus {
    Pending,
    Processing,
    Completed,
    Failed,
    Timeout
};

// 송금 결과
struct TransferResult {
    std::string transfer_id;
    TransferStatus status;
    std::string tx_hash;
    Duration elapsed;
    std::string error_message;
};

class TransferManager {
public:
    // 송금 시작
    std::future<Result<TransferResult>> initiate(const TransferRequest& req);
    
    // 상태 확인
    std::future<Result<TransferStatus>> check_status(const std::string& transfer_id);
    
    // 완료 대기 (폴링)
    std::future<Result<TransferResult>> wait_completion(
        const std::string& transfer_id,
        Duration timeout = std::chrono::minutes(30)
    );
    
private:
    // 거래소별 출금 API 호출
    Result<std::string> withdraw(Exchange ex, const TransferRequest& req);
    
    // 입금 확인
    Result<bool> check_deposit(Exchange ex, const std::string& tx_hash);
};
```

---

## ✅ 완료 조건

```
□ 출금 API 연동
□ Destination Tag 처리
□ 상태 추적
□ 타임아웃 처리
□ 입금 확인
```

---

## 📎 다음 태스크

완료 후: TASK_19_config_hotreload.md (Phase 3 인프라로 이동)
