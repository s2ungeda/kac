#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/spin_wait.hpp"
#include "arbitrage/common/logger.hpp"
#include <functional>
#include <cstring>
#include <atomic>

namespace arbitrage {

// =============================================================================
// 개별 주문 추적 엔트리
// =============================================================================
struct TrackedOrder {
    OrderUpdate last_update{};              // Private WS에서 수신한 최신 상태
    int64_t request_id{0};                  // DualOrderRequest의 request_id
    bool is_buy_side{false};                // buy/sell 구분
    bool registered{false};                 // register_order() 호출됨
    bool ws_received{false};                // Private WS 업데이트 수신됨
    bool active{false};                     // 이 슬롯 사용 중
    int64_t created_at_ms{0};               // 추적 시작 시각

    void reset() {
        last_update = OrderUpdate{};
        request_id = 0;
        is_buy_side = false;
        registered = false;
        ws_received = false;
        active = false;
        created_at_ms = 0;
    }
};

// =============================================================================
// 매수/매도 쌍 추적
// =============================================================================
struct DualOrderTrack {
    int64_t request_id{0};
    int buy_idx{-1};                        // tracked_orders_ 인덱스
    int sell_idx{-1};
    bool completed{false};
    bool active{false};

    void reset() {
        request_id = 0;
        buy_idx = -1;
        sell_idx = -1;
        completed = false;
        active = false;
    }
};

// =============================================================================
// OrderTracker: client_order_id 기반 주문 추적
// =============================================================================
// - DualOrderExecutor가 submit() 시 register_order() 호출
// - Private WebSocket이 OrderUpdate 수신 시 on_order_update() 호출
// - 양쪽 모두 terminal 상태가 되면 completion callback 호출
// - client_order_id 형식: "arb_{request_id}_{buy|sell}"
// =============================================================================
class OrderTracker {
public:
    // 양쪽 체결 완료 시 콜백
    using CompletionCallback = std::function<void(
        int64_t request_id,
        const TrackedOrder& buy,
        const TrackedOrder& sell)>;

    explicit OrderTracker(CompletionCallback on_complete = nullptr);

    // 콜백 설정
    void set_completion_callback(CompletionCallback cb) {
        on_complete_ = std::move(cb);
    }

    // DualOrderExecutor에서 호출 — 주문 제출 시 추적 등록
    // client_order_id에서 request_id와 side를 파싱하여 쌍으로 묶음
    void register_order(const char* client_order_id,
                        const char* exchange_order_id = nullptr);

    // Private WebSocket에서 호출 — 체결/취소 이벤트 수신
    // client_order_id로 매칭하여 상태 업데이트
    void on_order_update(const OrderUpdate& update);

    // 만료된 엔트리 정리 (주기적 호출)
    void cleanup_stale(int64_t max_age_ms = 300000);  // 5분

    // 현재 추적 중인 주문 수
    size_t active_count() const;

    // 특정 client_order_id의 현재 상태 조회
    const TrackedOrder* get_order(const char* client_order_id) const;

    // client_order_id 형식 파싱: "arb_{request_id}_{buy|sell}"
    static bool parse_client_order_id(const char* id,
                                       int64_t& request_id,
                                       bool& is_buy_side);

private:
    // client_order_id로 TrackedOrder 슬롯 찾기 (없으면 -1)
    int find_order(const char* client_order_id) const;

    // 빈 슬롯 할당
    int alloc_order();

    // request_id로 DualOrderTrack 찾기 (없으면 새로 할당)
    int find_or_alloc_dual(int64_t request_id);

    // 양쪽 완료 확인
    void check_completion(DualOrderTrack& track);

    // 현재 시각 (ms)
    static int64_t now_ms();

    // 고정 크기 풀 (new/delete 없음)
    TrackedOrder orders_[MAX_ORDER_TRACKER_ENTRIES * 2]{};
    DualOrderTrack duals_[MAX_ORDER_TRACKER_ENTRIES]{};

    CompletionCallback on_complete_;
    mutable SpinLock lock_;
    std::shared_ptr<SimpleLogger> logger_;
};

}  // namespace arbitrage
