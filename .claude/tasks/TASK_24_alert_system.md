# TASK 28: 알림 시스템 (C++)

## 🎯 목표
텔레그램/Discord 알림 발송

---

## 📁 생성할 파일

```
include/arbitrage/ops/
└── alert.hpp
src/ops/
└── alert.cpp
```

---

## 📝 핵심 구현

```cpp
enum class AlertLevel {
    Info,
    Warning,
    Error,
    Critical
};

struct Alert {
    AlertLevel level;
    std::string title;
    std::string message;
    std::chrono::system_clock::time_point timestamp;
};

class AlertService {
public:
    AlertService(const AlertConfig& config);
    
    // 알림 전송
    std::future<Result<void>> send(const Alert& alert);
    
    // 편의 메서드
    void info(const std::string& title, const std::string& msg);
    void warning(const std::string& title, const std::string& msg);
    void error(const std::string& title, const std::string& msg);
    void critical(const std::string& title, const std::string& msg);
    
private:
    Result<void> send_telegram(const Alert& alert);
    Result<void> send_discord(const Alert& alert);
    
    AlertConfig config_;
};
```

---

## ✅ 완료 조건

```
□ 텔레그램 봇 API
□ Discord Webhook
□ 레벨별 필터링
□ Rate Limit
□ 포맷팅
```

---

## 📎 다음 태스크

완료 후: TASK_29_daily_loss_limit.md
