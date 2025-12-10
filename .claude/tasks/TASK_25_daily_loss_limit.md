# TASK 29: 일일 손실 한도 (C++)

## 🎯 목표
일일 손실 한도 관리 및 킬스위치

---

## 📁 생성할 파일

```
include/arbitrage/ops/
└── daily_limit.hpp
src/ops/
└── daily_limit.cpp
```

---

## 📝 핵심 구현

```cpp
struct DailyStats {
    double realized_pnl{0.0};
    double unrealized_pnl{0.0};
    int trade_count{0};
    int win_count{0};
    std::chrono::system_clock::time_point reset_time;
};

class DailyLossLimiter {
public:
    DailyLossLimiter(double limit_krw, std::function<void()> kill_switch);
    
    // 손익 기록
    void record_trade(double pnl_krw);
    
    // 상태 조회
    DailyStats get_stats() const;
    double remaining_limit() const;
    bool is_limit_reached() const;
    
    // 수동 리셋
    void reset();
    
private:
    void check_and_trigger();
    void schedule_daily_reset();
    
    double limit_krw_;
    std::function<void()> kill_switch_;
    
    mutable std::mutex mutex_;
    DailyStats stats_;
    std::atomic<bool> triggered_{false};
};
```

---

## ✅ 완료 조건

```
□ 손익 추적
□ 한도 체크
□ 킬스위치 연동
□ 자정 리셋 (KST)
□ 통계
```

---

## 📎 다음 태스크

완료 후: TASK_21_multi_account.md
