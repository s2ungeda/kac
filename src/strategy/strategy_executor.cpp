/**
 * Strategy Executor Implementation (TASK_14)
 */

#include "arbitrage/strategy/strategy_executor.hpp"
#include "arbitrage/executor/dual_order.hpp"

#include <algorithm>
#include <iostream>

namespace arbitrage {

// =============================================================================
// 글로벌 인스턴스
// =============================================================================
StrategyExecutor& strategy_executor() {
    static StrategyExecutor instance;
    return instance;
}

// =============================================================================
// 생성자/소멸자
// =============================================================================
StrategyExecutor::StrategyExecutor(const StrategyExecutorConfig& config)
    : config_(config)
{
    current_snapshot_.set_timestamp_now();
}

StrategyExecutor::~StrategyExecutor() {
    stop();
}

// =============================================================================
// 전략 관리
// =============================================================================
bool StrategyExecutor::add_strategy(
    std::unique_ptr<IStrategy> strategy,
    const StrategyConfig& config
) {
    if (!strategy) return false;

    std::unique_lock lock(strategies_mutex_);

    StrategyId id = config.id;
    if (strategies_.find(id) != strategies_.end()) {
        return false;  // 이미 존재
    }

    strategy->initialize(config);
    strategies_[id] = std::move(strategy);
    configs_[id] = config;
    enabled_[id] = config.enabled;

    return true;
}

bool StrategyExecutor::add_strategy(const StrategyConfig& config) {
    auto strategy = StrategyRegistry::instance().create(config.type);
    if (!strategy) {
        return false;
    }
    return add_strategy(std::move(strategy), config);
}

bool StrategyExecutor::remove_strategy(const StrategyId& id) {
    std::unique_lock lock(strategies_mutex_);

    auto it = strategies_.find(id);
    if (it == strategies_.end()) {
        return false;
    }

    it->second->stop();
    strategies_.erase(it);
    configs_.erase(id);
    enabled_.erase(id);

    return true;
}

bool StrategyExecutor::enable_strategy(const StrategyId& id) {
    std::unique_lock lock(strategies_mutex_);

    auto it = strategies_.find(id);
    if (it == strategies_.end()) {
        return false;
    }

    enabled_[id] = true;
    it->second->start();
    return true;
}

bool StrategyExecutor::disable_strategy(const StrategyId& id) {
    std::unique_lock lock(strategies_mutex_);

    auto it = strategies_.find(id);
    if (it == strategies_.end()) {
        return false;
    }

    enabled_[id] = false;
    it->second->pause();
    return true;
}

IStrategy* StrategyExecutor::get_strategy(const StrategyId& id) {
    std::shared_lock lock(strategies_mutex_);
    auto it = strategies_.find(id);
    return it != strategies_.end() ? it->second.get() : nullptr;
}

const IStrategy* StrategyExecutor::get_strategy(const StrategyId& id) const {
    std::shared_lock lock(strategies_mutex_);
    auto it = strategies_.find(id);
    return it != strategies_.end() ? it->second.get() : nullptr;
}

std::vector<StrategyId> StrategyExecutor::strategy_ids() const {
    std::shared_lock lock(strategies_mutex_);
    std::vector<StrategyId> ids;
    ids.reserve(strategies_.size());
    for (const auto& [id, _] : strategies_) {
        ids.push_back(id);
    }
    return ids;
}

std::vector<StrategyId> StrategyExecutor::active_strategy_ids() const {
    std::shared_lock lock(strategies_mutex_);
    std::vector<StrategyId> ids;
    for (const auto& [id, enabled] : enabled_) {
        if (enabled) {
            ids.push_back(id);
        }
    }
    return ids;
}

// =============================================================================
// 실행 제어
// =============================================================================
void StrategyExecutor::start() {
    if (running_.load()) return;

    running_ = true;
    paused_ = false;

    // 모든 활성 전략 시작
    {
        std::shared_lock lock(strategies_mutex_);
        for (auto& [id, strategy] : strategies_) {
            if (enabled_[id]) {
                strategy->start();
            }
        }
    }

    // 실행 스레드 시작
    run_thread_ = std::thread(&StrategyExecutor::run_loop, this);
}

void StrategyExecutor::stop() {
    running_ = false;

    if (run_thread_.joinable()) {
        run_thread_.join();
    }

    // 모든 전략 정지
    std::shared_lock lock(strategies_mutex_);
    for (auto& [id, strategy] : strategies_) {
        strategy->stop();
    }
}

void StrategyExecutor::pause() {
    paused_ = true;

    std::shared_lock lock(strategies_mutex_);
    for (auto& [id, strategy] : strategies_) {
        strategy->pause();
    }
}

void StrategyExecutor::resume() {
    paused_ = false;

    std::shared_lock lock(strategies_mutex_);
    for (auto& [id, strategy] : strategies_) {
        if (enabled_[id]) {
            strategy->resume();
        }
    }
}

// =============================================================================
// 시장 데이터 수신
// =============================================================================
void StrategyExecutor::on_ticker_update(Exchange ex, const Ticker& ticker) {
    std::unique_lock lock(market_mutex_);
    current_snapshot_.tickers[static_cast<size_t>(ex)] = ticker;
    current_snapshot_.ticker_valid[static_cast<size_t>(ex)] = true;
    current_snapshot_.set_timestamp_now();
}

void StrategyExecutor::on_orderbook_update(Exchange ex, const OrderBook& ob) {
    std::unique_lock lock(market_mutex_);
    current_snapshot_.orderbooks[static_cast<size_t>(ex)] = ob;
    current_snapshot_.orderbook_valid[static_cast<size_t>(ex)] = true;
    current_snapshot_.set_timestamp_now();
}

void StrategyExecutor::on_fx_rate_update(double rate) {
    std::unique_lock lock(market_mutex_);
    current_snapshot_.fx_rate = rate;
}

void StrategyExecutor::on_premium_update(const PremiumMatrix& matrix) {
    std::unique_lock lock(market_mutex_);
    current_snapshot_.premium_matrix = matrix;
}

// =============================================================================
// 킬스위치
// =============================================================================
void StrategyExecutor::kill_switch(const char* reason) {
    kill_switch_ = true;

    {
        std::unique_lock lock(kill_reason_mutex_);
        kill_switch_reason_ = reason;
    }

    // 모든 전략 정지
    std::shared_lock lock(strategies_mutex_);
    for (auto& [id, strategy] : strategies_) {
        strategy->stop();
    }
}

void StrategyExecutor::reset_kill_switch() {
    kill_switch_ = false;

    {
        std::unique_lock lock(kill_reason_mutex_);
        kill_switch_reason_.clear();
    }
}

// =============================================================================
// 손익 추적
// =============================================================================
double StrategyExecutor::get_total_daily_pnl() const {
    double total = 0.0;
    std::shared_lock lock(strategies_mutex_);
    for (const auto& [id, strategy] : strategies_) {
        total += strategy->today_pnl();
    }
    return total;
}

void StrategyExecutor::reset_daily_pnl() {
    std::shared_lock lock(strategies_mutex_);
    for (auto& [id, strategy] : strategies_) {
        strategy->reset_stats();
    }
    stats_.reset();
}

// =============================================================================
// 설정
// =============================================================================
void StrategyExecutor::set_config(const StrategyExecutorConfig& config) {
    config_ = config;
}

// =============================================================================
// 실행 루프
// =============================================================================
void StrategyExecutor::run_loop() {
    while (running_.load()) {
        // 킬스위치 체크
        if (kill_switch_.load()) {
            std::this_thread::sleep_for(config_.eval_interval);
            continue;
        }

        // 일시정지 체크
        if (paused_.load()) {
            std::this_thread::sleep_for(config_.eval_interval);
            continue;
        }

        // 전략 평가
        evaluate_all();

        // 다음 평가까지 대기
        std::this_thread::sleep_for(config_.eval_interval);
    }
}

// =============================================================================
// 모든 전략 평가
// =============================================================================
void StrategyExecutor::evaluate_all() {
    // 현재 스냅샷 복사
    MarketSnapshot snapshot = create_snapshot();

    // 실행 요청 수집
    std::vector<std::pair<StrategyId, StrategyDecision>> execute_requests;

    {
        std::shared_lock lock(strategies_mutex_);

        for (auto& [id, strategy] : strategies_) {
            // 비활성화 전략 스킵
            if (!enabled_[id]) continue;

            // 전략 상태 확인
            if (strategy->state() != StrategyState::Running) continue;

            // 평가
            ++stats_.total_evaluations;
            StrategyDecision decision = strategy->evaluate(snapshot);

            // 콜백 호출
            if (decision_callback_) {
                decision_callback_(id, decision);
            }

            // 실행 요청 수집
            if (decision.should_execute()) {
                execute_requests.emplace_back(id, decision);
            }
        }
    }

    // 실행 요청이 없으면 종료
    if (execute_requests.empty()) return;

    // 충돌 해결 (여러 전략이 동시 실행 요청)
    if (execute_requests.size() > static_cast<size_t>(config_.max_concurrent_orders)) {
        ++stats_.conflicts_resolved;
        auto [id, decision] = resolve_conflicts(execute_requests);
        execute_decision(id, decision);
    } else {
        // 모든 요청 실행
        for (const auto& [id, decision] : execute_requests) {
            execute_decision(id, decision);
        }
    }
}

// =============================================================================
// 시장 스냅샷 생성
// =============================================================================
MarketSnapshot StrategyExecutor::create_snapshot() const {
    std::shared_lock lock(market_mutex_);
    return current_snapshot_;
}

// =============================================================================
// 충돌 해결
// =============================================================================
std::pair<StrategyId, StrategyDecision>
StrategyExecutor::resolve_conflicts(
    const std::vector<std::pair<StrategyId, StrategyDecision>>& decisions
) {
    if (decisions.empty()) {
        return {"", StrategyDecision::no_action("No decisions")};
    }

    if (decisions.size() == 1) {
        return decisions[0];
    }

    switch (config_.conflict_policy) {
        case ConflictPolicy::Priority: {
            // 우선순위 목록에서 가장 높은 전략 선택
            for (const auto& priority_id : config_.priority_order) {
                for (const auto& [id, decision] : decisions) {
                    if (id == priority_id) {
                        return {id, decision};
                    }
                }
            }
            // 우선순위 목록에 없으면 첫 번째 선택
            return decisions[0];
        }

        case ConflictPolicy::HighestProfit: {
            auto it = std::max_element(
                decisions.begin(), decisions.end(),
                [](const auto& a, const auto& b) {
                    return a.second.expected_profit_krw < b.second.expected_profit_krw;
                }
            );
            return *it;
        }

        case ConflictPolicy::HighestConfidence: {
            auto it = std::max_element(
                decisions.begin(), decisions.end(),
                [](const auto& a, const auto& b) {
                    return a.second.confidence < b.second.confidence;
                }
            );
            return *it;
        }

        case ConflictPolicy::RoundRobin: {
            size_t idx = round_robin_idx_.fetch_add(1) % decisions.size();
            return decisions[idx];
        }

        default:
            return decisions[0];
    }
}

// =============================================================================
// 주문 실행
// =============================================================================
void StrategyExecutor::execute_decision(
    const StrategyId& strategy_id,
    const StrategyDecision& decision
) {
    if (!decision.should_execute() || !decision.has_order_request) {
        return;
    }

    ++stats_.total_executions;

    // 일일 손실 한도 확인
    double daily_pnl = get_total_daily_pnl();
    if (daily_pnl < -config_.global_daily_loss_limit_krw) {
        kill_switch("Global daily loss limit reached");
        return;
    }

    // 주문 실행
    if (order_executor_) {
        auto result = order_executor_->execute_sync(decision.order_request);

        // 전략에 결과 피드백
        {
            std::shared_lock lock(strategies_mutex_);
            auto it = strategies_.find(strategy_id);
            if (it != strategies_.end()) {
                it->second->on_order_result(result);
            }
        }

        // 통계 업데이트
        if (result.both_filled()) {
            double profit = result.gross_profit(current_snapshot_.fx_rate);
            stats_.total_pnl_krw_x100.fetch_add(
                static_cast<int64_t>(profit * 100),
                std::memory_order_relaxed
            );
        }

        // 콜백 호출
        if (execution_callback_) {
            execution_callback_(strategy_id, result);
        }
    }
}

}  // namespace arbitrage
