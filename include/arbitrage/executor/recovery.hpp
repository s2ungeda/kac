#pragma once

#include "arbitrage/executor/types.hpp"
#include "arbitrage/exchange/order_base.hpp"
#include "arbitrage/common/spin_wait.hpp"
#include <map>
#include <memory>
#include <future>
#include <functional>
#include <queue>
#include <atomic>
#include <thread>

namespace arbitrage {

// =============================================================================
// 복구 관리자
// =============================================================================
// 부분 체결 시 자동 복구 로직 담당
//
// 판정 기준: 성공/실패 플래그가 아닌 양쪽 레그의 "실제 체결 수량" 차이.
// 양쪽 모두 부분 체결된 헤징 불일치(예: 매수 50 / 매도 40)도 잡아낸다.
//
// 복구 시나리오:
// 1. 매수 체결 > 매도 체결 → 초과분을 매수 거래소에서 손절 매도 (SellBought)
// 2. 매도 체결 > 매수 체결 → 부족분을 매도 거래소에서 재매수 (BuySold)
// 3. 체결량 일치 (전량/전무) → 복구 불필요 (None)
//
// 주의: REST 호출이 타임아웃 등으로 실패하면 실제 체결량을 알 수 없다.
// 이 경우 filled_qty=0으로 보고되므로, 호출측에서 주문 조회(멱등성 확인)
// 후 정확한 체결량으로 DualOrderResult를 갱신한 뒤 create_plan을 불러야 한다.
// =============================================================================
class RecoveryManager {
public:
    using RecoveryCompleteCallback = std::function<void(const RecoveryResult&)>;

    // 생성자
    // @param order_clients 거래소별 주문 클라이언트 맵
    RecoveryManager(
        std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients
    );

    ~RecoveryManager();

    // 복사/이동 금지
    RecoveryManager(const RecoveryManager&) = delete;
    RecoveryManager& operator=(const RecoveryManager&) = delete;

    // =========================================================================
    // 복구 계획 생성
    // =========================================================================

    // 동시 주문 결과를 분석하여 복구 계획 생성
    // @param request 원본 주문 요청
    // @param result 주문 결과
    // @return 복구 계획
    RecoveryPlan create_plan(
        const DualOrderRequest& request,
        const DualOrderResult& result
    );

    // =========================================================================
    // 복구 실행
    // =========================================================================

    // 동기 복구 실행
    // @param plan 복구 계획
    // @return 복구 결과
    RecoveryResult execute_recovery(const RecoveryPlan& plan);

    // 비동기 복구 실행
    // @param plan 복구 계획
    // @return 복구 결과 future
    std::future<RecoveryResult> execute_recovery_async(const RecoveryPlan& plan);

    // 콜백 방식 복구 실행
    // @param plan 복구 계획
    // @param callback 완료 콜백
    void execute_recovery_with_callback(
        const RecoveryPlan& plan,
        RecoveryCompleteCallback callback
    );

    // =========================================================================
    // 설정
    // =========================================================================

    // 최대 재시도 횟수 설정
    void set_max_retries(int max_retries) { max_retries_ = max_retries; }
    int max_retries() const { return max_retries_; }

    // 재시도 간격 설정
    void set_retry_delay(Duration delay) { retry_delay_ = delay; }
    Duration retry_delay() const { return retry_delay_; }

    // 손절 가격 슬리피지 허용률 (%)
    void set_slippage_tolerance(double pct) { slippage_tolerance_ = pct; }
    double slippage_tolerance() const { return slippage_tolerance_; }

    // 자동 복구 비활성화 (테스트용)
    void set_dry_run(bool enabled) { dry_run_ = enabled; }
    bool is_dry_run() const { return dry_run_; }

    // =========================================================================
    // 통계
    // =========================================================================

    struct Stats {
        std::atomic<uint64_t> total_plans{0};
        std::atomic<uint64_t> sell_bought_plans{0};
        std::atomic<uint64_t> buy_sold_plans{0};
        std::atomic<uint64_t> manual_plans{0};
        std::atomic<uint64_t> executions{0};
        std::atomic<uint64_t> successes{0};
        std::atomic<uint64_t> failures{0};
        std::atomic<uint64_t> retries{0};

        double success_rate() const {
            uint64_t exec = executions.load();
            return exec > 0 ?
                static_cast<double>(successes.load()) / exec * 100.0 : 0.0;
        }

        void reset() {
            total_plans = 0;
            sell_bought_plans = 0;
            buy_sold_plans = 0;
            manual_plans = 0;
            executions = 0;
            successes = 0;
            failures = 0;
            retries = 0;
        }
    };

    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

private:
    // 복구 계획 생성 헬퍼 (quantity = 복구할 수량)
    RecoveryPlan create_sell_bought_plan(
        const DualOrderRequest& request,
        double quantity,
        const std::string& reason
    );

    RecoveryPlan create_buy_sold_plan(
        const DualOrderRequest& request,
        double quantity,
        const std::string& reason
    );

    // 복구 주문 실행
    SingleOrderResult execute_order(const OrderRequest& order);

    // 재시도 로직
    RecoveryResult execute_with_retry(RecoveryPlan& plan);

    // 멤버 변수
    std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients_;
    Stats stats_;

    int max_retries_{3};
    Duration retry_delay_{Duration(100000)};  // 100ms
    double slippage_tolerance_{0.5};          // 0.5%
    bool dry_run_{false};
};

// =============================================================================
// 복구 큐 (비동기 복구 처리용)
// =============================================================================
class RecoveryQueue {
public:
    RecoveryQueue(std::shared_ptr<RecoveryManager> manager);
    ~RecoveryQueue();

    // 복구 요청 추가
    void enqueue(const RecoveryPlan& plan);

    // 큐 시작/중지
    void start();
    void stop();

    // 대기 중인 복구 수
    size_t pending_count() const;

    // 콜백 설정
    void set_callback(RecoveryManager::RecoveryCompleteCallback callback) {
        callback_ = std::move(callback);
    }

private:
    void worker_loop();

    std::shared_ptr<RecoveryManager> manager_;
    std::queue<RecoveryPlan> queue_;
    mutable SpinLock mutex_;
    std::atomic<bool> wakeup_{false};
    std::thread worker_;
    std::atomic<bool> running_{false};
    RecoveryManager::RecoveryCompleteCallback callback_;
};

}  // namespace arbitrage
