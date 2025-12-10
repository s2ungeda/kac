# TASK 36: 전략 플러그인 시스템 (C++)

## 🎯 목표
여러 매매 전략을 플러그인 방식으로 동시 운영 - 전략 추가/제거/활성화가 런타임에 가능한 구조

---

## ⚠️ 설계 원칙

### 왜 플러그인 방식인가?
```
1. 전략 다양화: 김프, 역김프, 삼각재정, Maker+Taker 등 다양한 로직
2. A/B 테스트: 동일 시장에서 여러 전략 성과 비교
3. 점진적 전환: 기존 전략 유지하며 신규 전략 테스트
4. 리스크 분산: 전략별 자본 할당
5. 핫스왑: 재시작 없이 전략 교체
```

### 전략 간 격리
```
각 전략은:
- 독립된 상태 관리
- 독립된 포지션/자본
- 독립된 리스크 한도
- 공유 데이터는 읽기 전용 (시세, 호가)
```

---

## 📁 생성할 파일

```
include/arbitrage/strategy/
├── strategy_interface.hpp      # 전략 인터페이스
├── strategy_registry.hpp       # 전략 등록/관리
├── strategy_executor.hpp       # 전략 실행 엔진
├── strategies/
│   ├── basic_arb_strategy.hpp  # 기본 김프 아비트라지
│   ├── maker_taker_strategy.hpp # Maker+Taker 전략
│   ├── reverse_arb_strategy.hpp # 역김프 전략
│   └── triangular_strategy.hpp  # 삼각 아비트라지
src/strategy/
├── strategy_registry.cpp
├── strategy_executor.cpp
├── strategies/
│   ├── basic_arb_strategy.cpp
│   ├── maker_taker_strategy.cpp
│   └── ...
config/
└── strategies.yaml
tests/unit/strategy/
└── strategy_plugin_test.cpp
```

---

## 📝 핵심 구현

### 1. 전략 인터페이스 (strategy_interface.hpp)

```cpp
#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/strategy/orderbook_analyzer.hpp"
#include "arbitrage/common/fee_calculator.hpp"

namespace arbitrage {

// 전략 ID
using StrategyId = std::string;

// 전략 상태
enum class StrategyState {
    Idle,           // 대기
    Analyzing,      // 분석 중
    Executing,      // 실행 중
    Paused,         // 일시정지
    Error           // 오류
};

// 시장 스냅샷 (읽기 전용)
struct MarketSnapshot {
    std::map<Exchange, Ticker> tickers;
    std::map<Exchange, OrderBook> orderbooks;
    PremiumMatrix premium_matrix;
    double fx_rate;
    std::chrono::system_clock::time_point timestamp;
};

// 전략 결정
struct StrategyDecision {
    enum class Action {
        None,           // 아무것도 안함
        Execute,        // 주문 실행
        Cancel,         // 기존 주문 취소
        Modify          // 주문 수정
    };
    
    Action action;
    std::string reason;
    double confidence;              // 0~1
    
    // Execute 시 주문 정보
    std::optional<DualOrderRequest> order_request;
    
    // 예상 수익
    double expected_profit_krw;
    double expected_profit_pct;
};

// 전략 설정 (YAML에서 로드)
struct StrategyConfig {
    StrategyId id;
    std::string type;               // "basic_arb", "maker_taker", etc.
    bool enabled;
    
    // 자본 할당
    double capital_allocation_pct;  // 전체 자본 중 할당 비율
    double max_position_krw;        // 최대 포지션
    
    // 리스크 한도
    double max_loss_per_trade_krw;
    double daily_loss_limit_krw;
    
    // 전략별 파라미터 (동적)
    std::map<std::string, double> params;
};

// ★ 전략 인터페이스 (순수 가상 클래스)
class IStrategy {
public:
    virtual ~IStrategy() = default;
    
    // 식별
    virtual StrategyId id() const = 0;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    
    // 라이프사이클
    virtual void initialize(const StrategyConfig& config) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    
    // ★ 핵심: 매 틱마다 호출되어 결정 반환
    virtual StrategyDecision evaluate(const MarketSnapshot& snapshot) = 0;
    
    // 주문 결과 피드백
    virtual void on_order_result(const DualOrderResult& result) = 0;
    virtual void on_transfer_result(const TransferResult& result) = 0;
    
    // 상태 조회
    virtual StrategyState state() const = 0;
    virtual double current_pnl() const = 0;
    virtual double today_pnl() const = 0;
    
    // 파라미터 동적 조정
    virtual void update_param(const std::string& key, double value) = 0;
    virtual std::map<std::string, double> get_params() const = 0;
    
    // 통계
    struct Stats {
        int total_trades;
        int winning_trades;
        double total_profit_krw;
        double max_drawdown_pct;
        double sharpe_ratio;
        std::chrono::system_clock::time_point last_trade_time;
    };
    virtual Stats get_stats() const = 0;
};

// 전략 팩토리 함수 타입
using StrategyFactory = std::function<std::unique_ptr<IStrategy>()>;

}  // namespace arbitrage
```

### 2. 전략 레지스트리 (strategy_registry.hpp)

```cpp
class StrategyRegistry {
public:
    static StrategyRegistry& instance();
    
    // 전략 타입 등록 (팩토리 패턴)
    void register_type(
        const std::string& type_name,
        StrategyFactory factory
    );
    
    // 전략 인스턴스 생성
    std::unique_ptr<IStrategy> create(const std::string& type_name);
    
    // 등록된 타입 목록
    std::vector<std::string> registered_types() const;
    
private:
    std::map<std::string, StrategyFactory> factories_;
};

// 매크로로 자동 등록
#define REGISTER_STRATEGY(TypeName, ClassName) \
    static bool _registered_##ClassName = []() { \
        StrategyRegistry::instance().register_type( \
            TypeName, \
            []() { return std::make_unique<ClassName>(); } \
        ); \
        return true; \
    }()
```

### 3. 전략 실행 엔진 (strategy_executor.hpp)

```cpp
class StrategyExecutor {
public:
    StrategyExecutor(
        std::shared_ptr<PremiumCalculator> premium,
        std::shared_ptr<OrderBookAnalyzer> ob_analyzer,
        std::shared_ptr<FeeCalculator> fee_calc,
        std::shared_ptr<DualOrderExecutor> order_exec
    );
    
    // 전략 로드/관리
    void load_strategies(const std::string& config_path);
    void add_strategy(std::unique_ptr<IStrategy> strategy, const StrategyConfig& config);
    void remove_strategy(const StrategyId& id);
    void enable_strategy(const StrategyId& id);
    void disable_strategy(const StrategyId& id);
    
    // 실행 루프
    void start();
    void stop();
    
    // 틱 처리 (이벤트 기반)
    void on_ticker_update(Exchange ex, const Ticker& ticker);
    void on_orderbook_update(Exchange ex, const OrderBook& ob);
    
    // 상태 조회
    std::vector<StrategyId> active_strategies() const;
    IStrategy* get_strategy(const StrategyId& id);
    
    // 전체 통계
    struct GlobalStats {
        double total_pnl_krw;
        int total_trades;
        std::map<StrategyId, IStrategy::Stats> per_strategy;
    };
    GlobalStats get_global_stats() const;
    
    // 킬스위치 (모든 전략 정지)
    void kill_switch(const std::string& reason);
    
private:
    void run_loop();
    void process_decisions(const std::vector<std::pair<StrategyId, StrategyDecision>>& decisions);
    MarketSnapshot create_snapshot() const;
    
    // 충돌 해결: 여러 전략이 동시에 실행 요청 시
    std::optional<std::pair<StrategyId, StrategyDecision>> 
    resolve_conflicts(const std::vector<std::pair<StrategyId, StrategyDecision>>& decisions);
    
private:
    std::map<StrategyId, std::unique_ptr<IStrategy>> strategies_;
    std::map<StrategyId, StrategyConfig> configs_;
    
    std::shared_ptr<PremiumCalculator> premium_;
    std::shared_ptr<OrderBookAnalyzer> ob_analyzer_;
    std::shared_ptr<FeeCalculator> fee_calc_;
    std::shared_ptr<DualOrderExecutor> order_exec_;
    
    std::jthread run_thread_;
    std::atomic<bool> running_{false};
    mutable std::shared_mutex mutex_;
};
```

### 4. 기본 전략 예시 (basic_arb_strategy.hpp)

```cpp
// 기본 김프 아비트라지 (Taker+Taker)
class BasicArbStrategy : public IStrategy {
public:
    StrategyId id() const override { return "basic_arb_1"; }
    std::string name() const override { return "Basic Kimchi Premium Arbitrage"; }
    
    StrategyDecision evaluate(const MarketSnapshot& snapshot) override {
        // 1. 최고 김프 기회 탐색
        auto best = find_best_opportunity(snapshot);
        if (!best) {
            return {StrategyDecision::Action::None, "No opportunity"};
        }
        
        // 2. 최소 프리미엄 체크
        if (best->premium_pct < params_["min_premium_pct"]) {
            return {StrategyDecision::Action::None, "Premium too low"};
        }
        
        // 3. 수수료 차감 후 순수익 체크
        auto cost = fee_calc_->calculate_arbitrage_cost(...);
        if (!cost.is_profitable()) {
            return {StrategyDecision::Action::None, "Not profitable after fees"};
        }
        
        // 4. 실행 결정
        return {
            StrategyDecision::Action::Execute,
            "Opportunity found",
            0.8,
            create_order_request(*best),
            cost.net_profit_krw,
            cost.net_profit_pct
        };
    }
    
private:
    std::map<std::string, double> params_ = {
        {"min_premium_pct", 2.0},
        {"max_position_xrp", 5000.0},
        {"max_slippage_bps", 20.0}
    };
};

REGISTER_STRATEGY("basic_arb", BasicArbStrategy);
```

### 5. Maker+Taker 전략 예시 (maker_taker_strategy.hpp)

```cpp
// Maker+Taker 전략 (수수료 최적화)
class MakerTakerStrategy : public IStrategy {
public:
    StrategyId id() const override { return "maker_taker_1"; }
    std::string name() const override { return "Maker+Taker Arbitrage"; }
    
    StrategyDecision evaluate(const MarketSnapshot& snapshot) override {
        // 1. 기회 탐색
        auto opp = find_opportunity(snapshot);
        if (!opp) return no_action();
        
        // 2. Maker 주문 대기 중인지 확인
        if (pending_maker_order_) {
            return handle_pending_maker();
        }
        
        // 3. 새 기회: Maker 가격 계산
        double maker_price = ob_analyzer_->calculate_optimal_maker_price(
            snapshot.orderbooks[opp->buy_exchange],
            OrderSide::Buy,
            params_["maker_fill_prob"],
            std::chrono::seconds(static_cast<int>(params_["maker_timeout_sec"]))
        );
        
        // 4. Maker 주문 먼저, Taker는 체결 후
        return {
            StrategyDecision::Action::Execute,
            "Maker order placement",
            0.7,
            create_maker_first_request(*opp, maker_price),
            estimated_profit,
            estimated_profit_pct
        };
    }
    
    void on_order_result(const DualOrderResult& result) override {
        // Maker 체결 확인 → Taker 실행
        if (result.is_maker_filled()) {
            execute_taker_leg();
        }
    }
    
private:
    std::map<std::string, double> params_ = {
        {"min_premium_pct", 1.5},      // Maker라 더 낮은 진입 가능
        {"maker_fill_prob", 0.8},
        {"maker_timeout_sec", 30.0},
        {"max_position_xrp", 10000.0}
    };
    
    std::optional<PendingOrder> pending_maker_order_;
};

REGISTER_STRATEGY("maker_taker", MakerTakerStrategy);
```

---

## 📊 전략 설정 파일 (strategies.yaml)

```yaml
strategies:
  - id: basic_arb_1
    type: basic_arb
    enabled: true
    capital_allocation_pct: 30
    max_position_krw: 10000000
    max_loss_per_trade_krw: 100000
    daily_loss_limit_krw: 500000
    params:
      min_premium_pct: 2.5
      max_slippage_bps: 20
      
  - id: maker_taker_1
    type: maker_taker
    enabled: true
    capital_allocation_pct: 50
    max_position_krw: 20000000
    max_loss_per_trade_krw: 150000
    daily_loss_limit_krw: 800000
    params:
      min_premium_pct: 1.5
      maker_fill_prob: 0.8
      maker_timeout_sec: 30
      
  - id: reverse_arb_1
    type: reverse_arb
    enabled: false                    # 역김프 시 활성화
    capital_allocation_pct: 20
    params:
      min_reverse_premium_pct: -2.0   # 음수 = 역김프

# 전략 간 충돌 해결 정책
conflict_resolution:
  policy: priority                    # priority | round_robin | highest_profit
  priority_order:
    - maker_taker_1
    - basic_arb_1
    - reverse_arb_1
```

---

## 🔗 의존성

```
TASK_07: PremiumCalculator
TASK_13: DualOrderExecutor
TASK_34: OrderBookAnalyzer
TASK_35: FeeCalculator
TASK_19: Config Hot-reload (전략 설정 갱신)
TASK_23: EventBus (시장 이벤트 수신)
```

---

## ✅ 완료 조건

```
□ 전략 인터페이스 (IStrategy)
  □ 라이프사이클 메서드
  □ evaluate() 틱 처리
  □ 통계 수집

□ 전략 레지스트리
  □ 팩토리 패턴 등록
  □ 동적 생성

□ 전략 실행 엔진
  □ 다중 전략 동시 실행
  □ 충돌 해결
  □ 킬스위치

□ 기본 전략 구현
  □ BasicArbStrategy
  □ MakerTakerStrategy
  □ (선택) ReverseArbStrategy
  
□ 설정 파일
  □ YAML 로드
  □ 런타임 갱신
  □ 전략별 파라미터

□ 스레드 안전
□ 단위 테스트
```

---

## 📎 다음 태스크

완료 후: TASK_17_ai_integration.md
