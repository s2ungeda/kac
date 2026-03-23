#pragma once

/**
 * TASK_43: OrderChannel — 양방향 SHM 주문 채널
 *
 * Engine ──DualOrderRequest──> OrderManager
 * Engine <──ShmDualOrderResult── OrderManager
 */

#include "arbitrage/ipc/ipc_types.hpp"
#include "arbitrage/ipc/shm_manager.hpp"
#include "arbitrage/ipc/shm_queue.hpp"
#include "arbitrage/executor/types.hpp"

#include <memory>
#include <cstring>
#include <chrono>

namespace arbitrage {

// DualOrderRequest trivially_copyable 검증
static_assert(std::is_trivially_copyable_v<DualOrderRequest>,
              "DualOrderRequest must be trivially copyable for SHM");

// =============================================================================
// 변환 유틸리티: in-process 타입 → SHM POD
// =============================================================================

inline ShmSingleOrderResult to_shm(const SingleOrderResult& src) {
    ShmSingleOrderResult dst{};
    dst.exchange = src.exchange;
    dst.success = src.is_success();
    dst.latency_us = src.latency.count();

    // steady_clock → system_clock 변환 (approximate)
    auto sys_now = std::chrono::system_clock::now();
    auto steady_now = std::chrono::steady_clock::now();
    auto offset = std::chrono::duration_cast<std::chrono::microseconds>(
        sys_now.time_since_epoch()) -
        std::chrono::duration_cast<std::chrono::microseconds>(
        steady_now.time_since_epoch());

    dst.start_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
        src.start_time.time_since_epoch()).count() + offset.count();
    dst.end_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
        src.end_time.time_since_epoch()).count() + offset.count();

    if (src.result.has_value()) {
        dst.status = src.result->status;
        std::strncpy(dst.order_id, src.result->order_id, MAX_ORDER_ID_LEN - 1);
        dst.order_id[MAX_ORDER_ID_LEN - 1] = '\0';
        dst.filled_qty = src.result->filled_qty;
        dst.avg_price = src.result->avg_price;
        dst.commission = src.result->commission;
    } else {
        dst.status = OrderStatus::Failed;
    }

    if (src.error.has_value()) {
        dst.error_code = static_cast<uint16_t>(src.error->code);
        std::strncpy(dst.error_message, src.error->message.c_str(),
                     MAX_MESSAGE_LEN - 1);
        dst.error_message[MAX_MESSAGE_LEN - 1] = '\0';
    }

    return dst;
}

inline ShmDualOrderResult to_shm(const DualOrderResult& src,
                                  double fx_rate = 1.0) {
    ShmDualOrderResult dst{};
    dst.buy_result = to_shm(src.buy_result);
    dst.sell_result = to_shm(src.sell_result);
    dst.request_id = src.request_id;
    dst.actual_premium = src.actual_premium;
    dst.gross_profit = src.gross_profit(fx_rate);
    dst.total_latency_us = src.total_latency().count();
    return dst;
}

// =============================================================================
// OrderChannel — 양방향 SHM 큐
// =============================================================================

class OrderChannel {
public:
    /**
     * Engine 측: SHM 생성 (request producer, result consumer)
     */
    static OrderChannel create_engine_side(size_t queue_capacity = 256) {
        OrderChannel ch;

        size_t req_size = shm_queue_size(queue_capacity, sizeof(DualOrderRequest));
        size_t res_size = shm_queue_size(queue_capacity, sizeof(ShmDualOrderResult));

        ch.request_shm_ = std::make_unique<ShmSegment>(
            shm_names::ORDERS, req_size, true);
        ch.result_shm_ = std::make_unique<ShmSegment>(
            shm_names::ORDER_RESULTS, res_size, true);

        if (!ch.request_shm_->valid() || !ch.result_shm_->valid()) {
            return OrderChannel();
        }

        ch.request_queue_ = ShmSPSCQueue<DualOrderRequest>::init_producer(
            ch.request_shm_->data(), queue_capacity);
        ch.result_queue_ = ShmSPSCQueue<ShmDualOrderResult>::attach_consumer(
            ch.result_shm_->data());
        // result_queue는 OrderManager가 init_producer하므로 여기서 attach 실패 가능
        // → OrderManager 시작 후 재연결 필요

        return ch;
    }

    /**
     * OrderManager 측: SHM 연결 (request consumer, result producer)
     */
    static OrderChannel create_order_manager_side(size_t queue_capacity = 256) {
        OrderChannel ch;

        size_t req_size = shm_queue_size(queue_capacity, sizeof(DualOrderRequest));
        size_t res_size = shm_queue_size(queue_capacity, sizeof(ShmDualOrderResult));

        ch.request_shm_ = std::make_unique<ShmSegment>(
            shm_names::ORDERS, req_size, false);
        ch.result_shm_ = std::make_unique<ShmSegment>(
            shm_names::ORDER_RESULTS, res_size, true);  // OrderManager가 result 생성

        if (!ch.request_shm_->valid() || !ch.result_shm_->valid()) {
            return OrderChannel();
        }

        ch.request_queue_ = ShmSPSCQueue<DualOrderRequest>::attach_consumer(
            ch.request_shm_->data());
        ch.result_queue_ = ShmSPSCQueue<ShmDualOrderResult>::init_producer(
            ch.result_shm_->data(), queue_capacity);

        return ch;
    }

    // Default constructor (invalid)
    OrderChannel() = default;

    // 이동 허용
    OrderChannel(OrderChannel&&) = default;
    OrderChannel& operator=(OrderChannel&&) = default;

    // Engine → OrderManager: 주문 요청 push
    bool push_request(const DualOrderRequest& req) {
        return request_queue_.push(req);
    }

    // OrderManager ← Engine: 주문 요청 pop
    bool pop_request(DualOrderRequest& req) {
        return request_queue_.pop(req);
    }

    // OrderManager → Engine: 주문 결과 push
    bool push_result(const ShmDualOrderResult& result) {
        return result_queue_.push(result);
    }

    // Engine ← OrderManager: 주문 결과 pop
    bool pop_result(ShmDualOrderResult& result) {
        return result_queue_.pop(result);
    }

    bool request_queue_valid() const { return request_queue_.valid(); }
    bool result_queue_valid() const { return result_queue_.valid(); }
    bool valid() const { return request_queue_.valid(); }

private:
    std::unique_ptr<ShmSegment> request_shm_;
    std::unique_ptr<ShmSegment> result_shm_;
    ShmSPSCQueue<DualOrderRequest> request_queue_;
    ShmSPSCQueue<ShmDualOrderResult> result_queue_;
};

}  // namespace arbitrage
