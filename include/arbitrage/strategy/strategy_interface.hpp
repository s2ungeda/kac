#pragma once

/**
 * Strategy Interface (TASK_14)
 *
 * 전략 플러그인 인터페이스 정의
 * - IStrategy: 전략 기본 인터페이스
 * - MarketSnapshot: 시장 데이터 스냅샷
 * - StrategyDecision: 전략 결정 결과
 * - StrategyConfig: 전략 설정
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/executor/types.hpp"

#include <string>
#include <map>
#include <vector>
#include <optional>
#include <chrono>
#include <functional>
#include <memory>
#include <atomic>

namespace arbitrage {

// 전방 선언
struct TransferResult;

// =============================================================================
// 전략 ID 타입
// =============================================================================
using StrategyId = std::string;

// =============================================================================
// 전략 상태
// =============================================================================
enum class StrategyState : uint8_t {
    Idle,           // 대기 (초기화됨)
    Running,        // 실행 중
    Analyzing,      // 분석 중
    Executing,      // 주문 실행 중
    Paused,         // 일시정지
    Stopped,        // 정지됨
    Error           // 오류 상태
};

constexpr const char* strategy_state_name(StrategyState state) {
    switch (state) {
        case StrategyState::Idle:      return "Idle";
        case StrategyState::Running:   return "Running";
        case StrategyState::Analyzing: return "Analyzing";
        case StrategyState::Executing: return "Executing";
        case StrategyState::Paused:    return "Paused";
        case StrategyState::Stopped:   return "Stopped";
        case StrategyState::Error:     return "Error";
        default:                       return "Unknown";
    }
}

// =============================================================================
// 시장 스냅샷 (읽기 전용)
// =============================================================================
struct MarketSnapshot {
    // 거래소별 시세
    Ticker tickers[static_cast<size_t>(Exchange::Count)];
    bool ticker_valid[static_cast<size_t>(Exchange::Count)]{};

    // 거래소별 오더북
    OrderBook orderbooks[static_cast<size_t>(Exchange::Count)];
    bool orderbook_valid[static_cast<size_t>(Exchange::Count)]{};

    // 프리미엄 매트릭스
    PremiumMatrix premium_matrix;

    // 환율
    double fx_rate{1400.0};

    // 타임스탬프
    int64_t timestamp_us{0};

    // 헬퍼 함수
    bool has_ticker(Exchange ex) const {
        return ticker_valid[static_cast<size_t>(ex)];
    }

    bool has_orderbook(Exchange ex) const {
        return orderbook_valid[static_cast<size_t>(ex)];
    }

    const Ticker& get_ticker(Exchange ex) const {
        return tickers[static_cast<size_t>(ex)];
    }

    const OrderBook& get_orderbook(Exchange ex) const {
        return orderbooks[static_cast<size_t>(ex)];
    }

    double get_premium(Exchange buy_ex, Exchange sell_ex) const {
        return premium_matrix[static_cast<int>(buy_ex)][static_cast<int>(sell_ex)];
    }

    void set_timestamp_now() {
        auto now = std::chrono::system_clock::now();
        timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
    }
};

// =============================================================================
// 전략 결정
// =============================================================================
struct StrategyDecision {
    enum class Action : uint8_t {
        None,           // 아무것도 안함
        Execute,        // 주문 실행
        Cancel,         // 기존 주문 취소
        Modify          // 주문 수정 (미래 확장용)
    };

    Action action{Action::None};

    // 결정 사유
    char reason[128]{};

    // 신뢰도 (0-1)
    double confidence{0.0};

    // Execute 시 주문 요청
    DualOrderRequest order_request;
    bool has_order_request{false};

    // 예상 수익
    double expected_profit_krw{0.0};
    double expected_profit_pct{0.0};

    // Cancel/Modify 시 대상 주문 ID
    char target_order_id[MAX_ORDER_ID_LEN]{};

    // 헬퍼 함수
    void set_reason(const char* r) {
        std::strncpy(reason, r, sizeof(reason) - 1);
        reason[sizeof(reason) - 1] = '\0';
    }

    void set_reason(const std::string& r) {
        set_reason(r.c_str());
    }

    bool should_execute() const { return action == Action::Execute; }
    bool should_cancel() const { return action == Action::Cancel; }
    bool is_none() const { return action == Action::None; }

    static StrategyDecision no_action(const char* reason = "No opportunity") {
        StrategyDecision d;
        d.action = Action::None;
        d.set_reason(reason);
        return d;
    }

    static StrategyDecision execute(
        const DualOrderRequest& request,
        double confidence,
        double expected_profit_krw,
        double expected_profit_pct,
        const char* reason = "Opportunity found"
    ) {
        StrategyDecision d;
        d.action = Action::Execute;
        d.order_request = request;
        d.has_order_request = true;
        d.confidence = confidence;
        d.expected_profit_krw = expected_profit_krw;
        d.expected_profit_pct = expected_profit_pct;
        d.set_reason(reason);
        return d;
    }
};

constexpr const char* decision_action_name(StrategyDecision::Action action) {
    switch (action) {
        case StrategyDecision::Action::None:    return "None";
        case StrategyDecision::Action::Execute: return "Execute";
        case StrategyDecision::Action::Cancel:  return "Cancel";
        case StrategyDecision::Action::Modify:  return "Modify";
        default:                                return "Unknown";
    }
}

// =============================================================================
// 전략 설정
// =============================================================================
struct StrategyParams {
    static constexpr int MAX_PARAMS = 16;

    struct Param {
        char key[32]{};
        double value{0.0};

        void set_key(const char* k) {
            std::strncpy(key, k, sizeof(key) - 1);
            key[sizeof(key) - 1] = '\0';
        }
    };

    Param params[MAX_PARAMS];
    int param_count{0};

    void set(const char* key, double value) {
        // 기존 키 찾기
        for (int i = 0; i < param_count; ++i) {
            if (std::strcmp(params[i].key, key) == 0) {
                params[i].value = value;
                return;
            }
        }
        // 새 키 추가
        if (param_count < MAX_PARAMS) {
            params[param_count].set_key(key);
            params[param_count].value = value;
            ++param_count;
        }
    }

    double get(const char* key, double default_value = 0.0) const {
        for (int i = 0; i < param_count; ++i) {
            if (std::strcmp(params[i].key, key) == 0) {
                return params[i].value;
            }
        }
        return default_value;
    }

    bool has(const char* key) const {
        for (int i = 0; i < param_count; ++i) {
            if (std::strcmp(params[i].key, key) == 0) {
                return true;
            }
        }
        return false;
    }
};

struct StrategyConfig {
    // 식별
    char id[32]{};
    char type[32]{};
    bool enabled{true};

    // 자본 할당
    double capital_allocation_pct{10.0};  // 전체 자본 중 할당 비율
    double max_position_krw{10000000.0};  // 최대 포지션 (1천만원)

    // 리스크 한도
    double max_loss_per_trade_krw{100000.0};  // 건당 최대 손실 (10만원)
    double daily_loss_limit_krw{500000.0};    // 일일 손실 한도 (50만원)

    // 전략별 파라미터
    StrategyParams params;

    // 헬퍼 함수
    void set_id(const char* i) {
        std::strncpy(id, i, sizeof(id) - 1);
        id[sizeof(id) - 1] = '\0';
    }

    void set_type(const char* t) {
        std::strncpy(type, t, sizeof(type) - 1);
        type[sizeof(type) - 1] = '\0';
    }
};

// =============================================================================
// 전략 통계
// =============================================================================
struct StrategyStats {
    std::atomic<int64_t> total_trades{0};
    std::atomic<int64_t> winning_trades{0};
    std::atomic<int64_t> losing_trades{0};
    std::atomic<int64_t> total_profit_krw_x100{0};  // 소수점 2자리 정밀도
    std::atomic<int64_t> max_profit_krw_x100{0};
    std::atomic<int64_t> max_loss_krw_x100{0};
    std::atomic<int64_t> total_volume_krw_x100{0};
    int64_t last_trade_timestamp_us{0};

    void record_trade(double profit_krw, double volume_krw) {
        ++total_trades;
        if (profit_krw > 0) {
            ++winning_trades;
        } else if (profit_krw < 0) {
            ++losing_trades;
        }

        int64_t profit_x100 = static_cast<int64_t>(profit_krw * 100);
        total_profit_krw_x100.fetch_add(profit_x100, std::memory_order_relaxed);
        total_volume_krw_x100.fetch_add(
            static_cast<int64_t>(volume_krw * 100), std::memory_order_relaxed);

        // Max profit/loss 업데이트
        if (profit_krw > 0) {
            int64_t current = max_profit_krw_x100.load();
            while (profit_x100 > current &&
                   !max_profit_krw_x100.compare_exchange_weak(current, profit_x100));
        } else {
            int64_t current = max_loss_krw_x100.load();
            while (profit_x100 < current &&
                   !max_loss_krw_x100.compare_exchange_weak(current, profit_x100));
        }

        last_trade_timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    double total_profit_krw() const {
        return total_profit_krw_x100.load() / 100.0;
    }

    double win_rate() const {
        int64_t total = total_trades.load();
        return total > 0 ? static_cast<double>(winning_trades.load()) / total * 100.0 : 0.0;
    }

    void reset() {
        total_trades = 0;
        winning_trades = 0;
        losing_trades = 0;
        total_profit_krw_x100 = 0;
        max_profit_krw_x100 = 0;
        max_loss_krw_x100 = 0;
        total_volume_krw_x100 = 0;
        last_trade_timestamp_us = 0;
    }
};

// =============================================================================
// 전략 인터페이스 (순수 가상 클래스)
// =============================================================================
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // =========================================================================
    // 식별
    // =========================================================================
    virtual const char* id() const = 0;
    virtual const char* name() const = 0;
    virtual const char* description() const = 0;

    // =========================================================================
    // 라이프사이클
    // =========================================================================
    virtual void initialize(const StrategyConfig& config) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;

    // =========================================================================
    // 핵심: 매 틱마다 호출되어 결정 반환
    // =========================================================================
    virtual StrategyDecision evaluate(const MarketSnapshot& snapshot) = 0;

    // =========================================================================
    // 주문 결과 피드백
    // =========================================================================
    virtual void on_order_result(const DualOrderResult& result) = 0;

    // =========================================================================
    // 상태 조회
    // =========================================================================
    virtual StrategyState state() const = 0;
    virtual double current_pnl() const = 0;
    virtual double today_pnl() const = 0;

    // =========================================================================
    // 파라미터 동적 조정
    // =========================================================================
    virtual void update_param(const char* key, double value) = 0;
    virtual double get_param(const char* key) const = 0;

    // =========================================================================
    // 통계
    // =========================================================================
    virtual const StrategyStats& stats() const = 0;
    virtual void reset_stats() = 0;
};

// =============================================================================
// 전략 팩토리 함수 타입
// =============================================================================
using StrategyFactory = std::function<std::unique_ptr<IStrategy>()>;

// =============================================================================
// 전략 기본 구현 (공통 로직)
// =============================================================================
class StrategyBase : public IStrategy {
public:
    StrategyBase() = default;
    ~StrategyBase() override = default;

    // 식별 (서브클래스에서 오버라이드)
    const char* id() const override { return config_.id; }

    // 라이프사이클
    void initialize(const StrategyConfig& config) override {
        config_ = config;
        state_ = StrategyState::Idle;
    }

    void start() override {
        if (state_ == StrategyState::Idle || state_ == StrategyState::Stopped) {
            state_ = StrategyState::Running;
        }
    }

    void stop() override {
        state_ = StrategyState::Stopped;
    }

    void pause() override {
        if (state_ == StrategyState::Running) {
            state_ = StrategyState::Paused;
        }
    }

    void resume() override {
        if (state_ == StrategyState::Paused) {
            state_ = StrategyState::Running;
        }
    }

    // 상태 조회
    StrategyState state() const override { return state_; }

    double current_pnl() const override {
        return stats_.total_profit_krw();
    }

    double today_pnl() const override {
        return today_pnl_.load() / 100.0;
    }

    // 파라미터
    void update_param(const char* key, double value) override {
        config_.params.set(key, value);
    }

    double get_param(const char* key) const override {
        return config_.params.get(key, 0.0);
    }

    // 통계
    const StrategyStats& stats() const override { return stats_; }
    void reset_stats() override {
        stats_.reset();
        today_pnl_ = 0;
    }

    // 주문 결과 기본 처리
    void on_order_result(const DualOrderResult& result) override {
        if (result.both_filled()) {
            double profit = result.gross_profit(config_.params.get("fx_rate", 1400.0));
            double volume = result.total_buy_cost(config_.params.get("fx_rate", 1400.0));
            stats_.record_trade(profit, volume);
            today_pnl_.fetch_add(static_cast<int64_t>(profit * 100), std::memory_order_relaxed);
        }
    }

protected:
    StrategyConfig config_;
    StrategyState state_{StrategyState::Idle};
    mutable StrategyStats stats_;
    std::atomic<int64_t> today_pnl_{0};
};

}  // namespace arbitrage
