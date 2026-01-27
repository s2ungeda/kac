#pragma once

#include "arbitrage/executor/types.hpp"
#include "arbitrage/exchange/order_base.hpp"
#include <map>
#include <memory>
#include <future>
#include <functional>

namespace arbitrage {

// Forward declaration
class RecoveryManager;

// =============================================================================
// 동시 주문 실행기 콜백
// =============================================================================
using DualOrderCallback = std::function<void(const DualOrderResult&)>;
using RecoveryCallback = std::function<void(const RecoveryResult&)>;

// =============================================================================
// 동시 주문 실행기
// =============================================================================
// 아비트라지를 위한 두 거래소 동시 주문 실행
//
// 핵심 원칙:
// 1. std::async 병렬 실행으로 동시 주문
// 2. 부분 체결 감지 및 자동 복구
// 3. 지연 시간 측정 및 통계 수집
// =============================================================================
class DualOrderExecutor {
public:
    // 생성자
    // @param order_clients 거래소별 주문 클라이언트 맵
    // @param recovery 복구 관리자 (nullptr이면 복구 안 함)
    DualOrderExecutor(
        std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients,
        std::shared_ptr<RecoveryManager> recovery = nullptr
    );

    ~DualOrderExecutor();

    // 복사/이동 금지
    DualOrderExecutor(const DualOrderExecutor&) = delete;
    DualOrderExecutor& operator=(const DualOrderExecutor&) = delete;

    // =========================================================================
    // 주문 실행
    // =========================================================================

    // 동기 실행 (블로킹)
    // @param request 동시 주문 요청
    // @return 동시 주문 결과 (양쪽 모두 완료 후 반환)
    DualOrderResult execute_sync(const DualOrderRequest& request);

    // 비동기 실행
    // @param request 동시 주문 요청
    // @return 결과를 담은 future
    std::future<DualOrderResult> execute_async(const DualOrderRequest& request);

    // 콜백 방식 실행 (non-blocking)
    // @param request 동시 주문 요청
    // @param callback 완료 시 호출할 콜백
    void execute_with_callback(
        const DualOrderRequest& request,
        DualOrderCallback callback
    );

    // =========================================================================
    // 설정
    // =========================================================================

    // 복구 관리자 설정
    void set_recovery_manager(std::shared_ptr<RecoveryManager> recovery);

    // 자동 복구 활성화/비활성화
    void set_auto_recovery(bool enabled) { auto_recovery_ = enabled; }
    bool is_auto_recovery() const { return auto_recovery_; }

    // 복구 콜백 설정
    void set_recovery_callback(RecoveryCallback callback) {
        recovery_callback_ = std::move(callback);
    }

    // 타임아웃 설정 (개별 주문)
    void set_order_timeout(Duration timeout) { order_timeout_ = timeout; }
    Duration order_timeout() const { return order_timeout_; }

    // =========================================================================
    // 통계
    // =========================================================================

    const ExecutorStats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

    // =========================================================================
    // 상태 확인
    // =========================================================================

    // 특정 거래소 주문 클라이언트 사용 가능 여부
    bool has_exchange(Exchange ex) const {
        return order_clients_.find(ex) != order_clients_.end();
    }

    // 지원하는 거래소 목록
    std::vector<Exchange> supported_exchanges() const;

private:
    // 개별 주문 실행
    SingleOrderResult execute_single(
        Exchange exchange,
        const OrderRequest& order,
        Duration delay
    );

    // 복구 처리
    void handle_recovery(
        const DualOrderRequest& request,
        DualOrderResult& result
    );

    // 멤버 변수
    std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients_;
    std::shared_ptr<RecoveryManager> recovery_;
    ExecutorStats stats_;

    bool auto_recovery_{true};
    Duration order_timeout_{Duration(30000000)};  // 30초

    DualOrderCallback result_callback_;
    RecoveryCallback recovery_callback_;
};

}  // namespace arbitrage
