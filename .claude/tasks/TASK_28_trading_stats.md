# TASK 32: 거래 통계 (C++)

## 🎯 목표
거래 실적 통계 및 리포팅

---

## 📁 생성할 파일

```
include/arbitrage/ops/
└── trading_stats.hpp
src/ops/
└── trading_stats.cpp
```

---

## 📝 핵심 구현

```cpp
struct TradingStats {
    // 기간
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    
    // 거래
    int total_trades{0};
    int winning_trades{0};
    int losing_trades{0};
    
    // 손익
    double total_profit_krw{0.0};
    double total_loss_krw{0.0};
    double net_pnl_krw{0.0};
    
    // 분석
    double win_rate() const;
    double profit_factor() const;
    double avg_profit() const;
    double avg_loss() const;
    double max_drawdown() const;
    double sharpe_ratio() const;
};

class TradingStatsTracker {
public:
    // 거래 기록
    void record_trade(const TradeRecord& trade);
    
    // 통계 조회
    TradingStats get_daily_stats();
    TradingStats get_weekly_stats();
    TradingStats get_monthly_stats();
    TradingStats get_all_time_stats();
    
    // DB 저장/로드
    void save_to_db();
    void load_from_db();
    
private:
    std::vector<TradeRecord> trades_;
    SQLite::Database db_;
};
```

---

## ✅ 완료 조건

```
□ 거래 기록
□ 일/주/월 통계
□ 승률/손익비
□ 드로다운
□ DB 저장
```

---

## 📎 다음 태스크

완료 후: TASK_33_web_dashboard.md
