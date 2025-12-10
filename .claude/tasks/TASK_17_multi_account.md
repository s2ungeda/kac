# TASK 21: 다중 계정 지원 (C++)

## 🎯 목표
거래소당 복수 계정 관리

---

## 📁 생성할 파일

```
include/arbitrage/common/
└── account_manager.hpp
src/common/
└── account_manager.cpp
```

---

## 📝 핵심 구현

```cpp
struct Account {
    std::string id;
    Exchange exchange;
    std::string api_key;
    std::string api_secret;
    bool enabled{true};
    double weight{1.0};  // 주문 분배 가중치
};

class AccountManager {
public:
    // 계정 관리
    void add_account(const Account& account);
    void remove_account(const std::string& id);
    std::vector<Account> get_accounts(Exchange ex) const;
    
    // 최적 계정 선택
    std::optional<Account> select_account(
        Exchange ex, 
        double required_balance
    );
    
    // 잔고 조회
    std::map<std::string, double> get_total_balance(Exchange ex);
};
```

---

## ✅ 완료 조건

```
□ 계정 등록/삭제
□ 가중치 기반 선택
□ 잔고 통합 조회
```

---

## 📎 다음 태스크

완료 후: TASK_38_watchdog.md
