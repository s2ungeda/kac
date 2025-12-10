# TASK 18: 통합 테스트 (C++)

## 🎯 목표
전략 엔진 통합 테스트 및 시뮬레이션

---

## 📁 생성할 파일

```
tests/integration/
├── strategy_test.cpp
├── mock_exchange.hpp
└── simulator.hpp
```

---

## 📝 핵심 구현

```cpp
// Mock 거래소
class MockExchange : public IExchange {
public:
    // 시세 설정
    void set_price(double price);
    
    // 주문 결과 설정
    void set_order_result(const OrderResult& result);
    void set_order_latency(Duration latency);
    void set_failure_rate(double rate);
    
    // IExchange 구현...
};

// 시뮬레이터
class ArbitrageSimulator {
public:
    void set_initial_balances(std::map<Exchange, double> balances);
    void set_fx_rate(double rate);
    
    // 시뮬레이션 실행
    SimulationResult run(
        const std::vector<PremiumInfo>& opportunities,
        int max_trades = 100
    );
};
```

---

## ✅ 완료 조건

```
□ Mock 거래소
□ 전략 테스트
□ 시뮬레이션
□ 성능 측정
□ 에지 케이스
```

---

## 📎 다음 태스크

완료 후: TASK_25_health_check.md (Phase 5 서버로 이동)
