# TASK 16: 의사결정 엔진 (C++)

## 🎯 목표
아비트라지 기회 평가 및 실행 결정

---

## 📁 생성할 파일

```
include/arbitrage/strategy/
└── decision_engine.hpp
src/strategy/
└── decision_engine.cpp
```

---

## 📝 핵심 구현

```cpp
// 결정 결과
enum class Decision {
    Execute,      // 실행
    Skip,         // 스킵 (리스크)
    Wait,         // 대기 (조건 미충족)
    HoldOff       // 보류 (킬스위치 등)
};

struct DecisionResult {
    Decision decision;
    std::string reason;
    DualOrderRequest order_request;  // Execute 시
    double confidence;               // 신뢰도 0-1
};

class DecisionEngine {
public:
    DecisionEngine(
        std::shared_ptr<PremiumCalculator> premium,
        std::shared_ptr<RiskModel> risk,
        const StrategyConfig& config
    );
    
    // 기회 평가
    DecisionResult evaluate(const PremiumInfo& opportunity);
    
    // 최적 수량 계산
    double calculate_optimal_qty(const PremiumInfo& opp);
    
    // 킬스위치 상태
    void set_kill_switch(bool active);
    bool is_kill_switch_active() const;
    
private:
    bool check_preconditions();
    bool check_risk_limits(const RiskAssessment& risk);
    double apply_position_sizing(double base_qty);
};
```

---

## ✅ 완료 조건

```
□ 기회 평가 로직
□ 수량 결정
□ 리스크 검증
□ 킬스위치 연동
□ 결정 로깅
```

---

## 📎 다음 태스크

완료 후: TASK_36_strategy_plugin.md
