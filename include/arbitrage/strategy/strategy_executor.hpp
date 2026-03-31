#pragma once

/**
 * Strategy Executor (TASK_14)
 *
 * 다중 전략 동시 실행 엔진
 * - 전략 로드/관리
 * - 시장 데이터 수신 및 배포
 * - 충돌 해결
 * - 킬스위치
 */

#include "arbitrage/strategy/strategy_interface.hpp"
#include "arbitrage/strategy/strategy_registry.hpp"
#include "arbitrage/common/spin_wait.hpp"

#include <map>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>

namespace arbitrage {

// 전방 선언
class DualOrderExecutor;

// =============================================================================
// 충돌 해결 정책
// =============================================================================
enum class ConflictPolicy : uint8_t {
    Priority,       // 우선순위 기반 (첫 번째 전략 우선)
    HighestProfit,  // 가장 높은 예상 수익
    HighestConfidence,  // 가장 높은 신뢰도
    RoundRobin      // 라운드 로빈
};

constexpr const char* conflict_policy_name(ConflictPolicy policy) {
    switch (policy) {
        case ConflictPolicy::Priority:        return "Priority";
        case ConflictPolicy::HighestProfit:   return "HighestProfit";
        case ConflictPolicy::HighestConfidence: return "HighestConfidence";
        case ConflictPolicy::RoundRobin:      return "RoundRobin";
        default:                              return "Unknown";
    }
}

// =============================================================================
// 전략 실행기 설정
// =============================================================================
struct StrategyExecutorConfig {
    // 실행 루프 설정
    std::chrono::milliseconds eval_interval{100};  // 평가 주기

    // 충돌 해결
    ConflictPolicy conflict_policy{ConflictPolicy::Priority};

    // 킬스위치 설정
    double global_daily_loss_limit_krw{1000000.0};  // 전체 일일 손실 한도

    // 동시 실행 제한
    int max_concurrent_orders{1};

    // 우선순위 목록 (Priority 정책 시 사용)
    std::vector<StrategyId> priority_order;
};

// =============================================================================
// 전략 실행기
// =============================================================================
class StrategyExecutor {
public:
    explicit StrategyExecutor(const StrategyExecutorConfig& config = {});
    ~StrategyExecutor();

    // 복사/이동 금지
    StrategyExecutor(const StrategyExecutor&) = delete;
    StrategyExecutor& operator=(const StrategyExecutor&) = delete;

    // =========================================================================
    // 전략 관리
    // =========================================================================

    /**
     * 전략 추가
     * @param strategy 전략 인스턴스
     * @param config 전략 설정
     * @return 성공 여부
     */
    bool add_strategy(std::unique_ptr<IStrategy> strategy, const StrategyConfig& config);

    /**
     * 설정으로 전략 생성 및 추가
     * @param config 전략 설정 (type 필드로 레지스트리에서 생성)
     * @return 성공 여부
     */
    bool add_strategy(const StrategyConfig& config);

    /**
     * 전략 제거
     */
    bool remove_strategy(const StrategyId& id);

    /**
     * 전략 활성화
     */
    bool enable_strategy(const StrategyId& id);

    /**
     * 전략 비활성화
     */
    bool disable_strategy(const StrategyId& id);

    /**
     * 전략 조회
     */
    IStrategy* get_strategy(const StrategyId& id);
    const IStrategy* get_strategy(const StrategyId& id) const;

    /**
     * 모든 전략 ID 조회
     */
    std::vector<StrategyId> strategy_ids() const;

    /**
     * 활성 전략 ID 조회
     */
    std::vector<StrategyId> active_strategy_ids() const;

    // =========================================================================
    // 실행 제어
    // =========================================================================

    /**
     * 실행 시작
     */
    void start();

    /**
     * 실행 중지
     */
    void stop();

    /**
     * 일시정지
     */
    void pause();

    /**
     * 재개
     */
    void resume();

    /**
     * 실행 중 여부
     */
    bool is_running() const { return running_.load(); }

    // =========================================================================
    // 시장 데이터 수신 (이벤트 기반)
    // =========================================================================

    /**
     * 시세 업데이트
     */
    void on_ticker_update(Exchange ex, const Ticker& ticker);

    /**
     * 오더북 업데이트
     */
    void on_orderbook_update(Exchange ex, const OrderBook& ob);

    /**
     * 환율 업데이트
     */
    void on_fx_rate_update(double rate);

    /**
     * 프리미엄 매트릭스 업데이트
     */
    void on_premium_update(const PremiumMatrix& matrix);

    // =========================================================================
    // 주문 실행기 연결
    // =========================================================================

    void set_order_executor(DualOrderExecutor* executor) {
        order_executor_ = executor;
    }

    // =========================================================================
    // 킬스위치
    // =========================================================================

    /**
     * 킬스위치 활성화 (모든 전략 정지)
     */
    void kill_switch(const char* reason);

    /**
     * 킬스위치 상태
     */
    bool is_kill_switch_active() const { return kill_switch_.load(); }

    /**
     * 킬스위치 해제
     */
    void reset_kill_switch();

    // =========================================================================
    // 손익 추적
    // =========================================================================

    /**
     * 전체 일일 손익
     */
    double get_total_daily_pnl() const;

    /**
     * 일일 손익 리셋
     */
    void reset_daily_pnl();

    // =========================================================================
    // 설정
    // =========================================================================

    void set_config(const StrategyExecutorConfig& config);
    const StrategyExecutorConfig& config() const { return config_; }

    // =========================================================================
    // 통계
    // =========================================================================

    struct GlobalStats {
        std::atomic<uint64_t> total_evaluations{0};
        std::atomic<uint64_t> total_executions{0};
        std::atomic<uint64_t> conflicts_resolved{0};
        std::atomic<int64_t> total_pnl_krw_x100{0};

        double total_pnl_krw() const {
            return total_pnl_krw_x100.load() / 100.0;
        }

        void reset() {
            total_evaluations = 0;
            total_executions = 0;
            conflicts_resolved = 0;
            total_pnl_krw_x100 = 0;
        }
    };

    const GlobalStats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

    // =========================================================================
    // 콜백
    // =========================================================================

    using DecisionCallback = std::function<void(const StrategyId&, const StrategyDecision&)>;
    using ExecutionCallback = std::function<void(const StrategyId&, const DualOrderResult&)>;

    void set_decision_callback(DecisionCallback cb) { decision_callback_ = std::move(cb); }
    void set_execution_callback(ExecutionCallback cb) { execution_callback_ = std::move(cb); }

private:
    // 평가 루프
    void run_loop();

    // 모든 전략 평가
    void evaluate_all();

    // 현재 시장 스냅샷 생성
    MarketSnapshot create_snapshot() const;

    // 충돌 해결
    std::pair<StrategyId, StrategyDecision>
    resolve_conflicts(const std::vector<std::pair<StrategyId, StrategyDecision>>& decisions);

    // 주문 실행
    void execute_decision(const StrategyId& strategy_id, const StrategyDecision& decision);

    // 멤버 변수
    StrategyExecutorConfig config_;

    // 전략 저장소
    mutable RWSpinLock strategies_mutex_;
    std::map<StrategyId, std::unique_ptr<IStrategy>> strategies_;
    std::map<StrategyId, StrategyConfig> configs_;
    std::map<StrategyId, bool> enabled_;

    // 시장 데이터
    mutable RWSpinLock market_mutex_;
    MarketSnapshot current_snapshot_;

    // 실행 제어
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> kill_switch_{false};
    std::thread run_thread_;

    // 외부 참조
    DualOrderExecutor* order_executor_{nullptr};

    // 라운드 로빈 인덱스
    std::atomic<size_t> round_robin_idx_{0};

    // 통계
    mutable GlobalStats stats_;

    // 콜백
    DecisionCallback decision_callback_;
    ExecutionCallback execution_callback_;

    // 킬스위치 사유
    mutable RWSpinLock kill_reason_mutex_;
    std::string kill_switch_reason_;
};

// =============================================================================
// 글로벌 인스턴스 접근자
// =============================================================================
StrategyExecutor& strategy_executor();

}  // namespace arbitrage
