# TASK 15: 리스크 모델 (C++)

## 🎯 목표
송금 리스크 및 시장 리스크 모델링

---

## 📁 생성할 파일

```
include/arbitrage/strategy/
└── risk_model.hpp
src/strategy/
└── risk_model.cpp
```

---

## 📝 핵심 구현

```cpp
// 리스크 평가 결과
struct RiskAssessment {
    double score;              // 0-100 (높을수록 위험)
    double expected_profit;    // 예상 수익
    double max_loss;           // 최대 손실
    double var_95;             // 95% VaR
    bool is_acceptable;        // 허용 가능 여부
    std::vector<std::string> warnings;
};

class RiskModel {
public:
    // 아비트라지 리스크 평가
    RiskAssessment evaluate(
        const PremiumInfo& opportunity,
        double order_qty,
        Duration estimated_transfer_time
    );
    
    // 송금 리스크 계산
    double calculate_transfer_risk(
        Exchange from,
        Exchange to,
        Duration transfer_time
    );
    
    // 슬리피지 예상
    double estimate_slippage(Exchange ex, double qty);
    
    // 김프 변동성 계산
    double calculate_premium_volatility();
    
private:
    // 과거 김프 데이터
    std::deque<double> premium_history_;
    static constexpr size_t HISTORY_SIZE = 1000;
};
```

---

## ✅ 완료 조건

```
□ 송금 리스크 계산
□ 슬리피지 예상
□ 김프 변동성
□ VaR 계산
□ 종합 리스크 점수
```

---

## 📎 다음 태스크

완료 후: TASK_16_decision_engine.md
