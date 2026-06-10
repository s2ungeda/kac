#include "arbitrage/executor/order_tracker.hpp"
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <chrono>

namespace arbitrage {

OrderTracker::OrderTracker(CompletionCallback on_complete)
    : on_complete_(std::move(on_complete))
    , logger_(Logger::get("OrderTracker"))
{
}

// =============================================================================
// client_order_id 파싱: "arb_{request_id}_{buy|sell}"
// =============================================================================

bool OrderTracker::parse_client_order_id(const char* id,
                                          int64_t& request_id,
                                          bool& is_buy_side) {
    // 형식: "arb_{request_id}_{buy|sell}"
    if (std::strncmp(id, "arb_", 4) != 0) {
        return false;
    }

    const char* num_start = id + 4;
    const char* underscore = std::strchr(num_start, '_');
    if (!underscore) {
        return false;
    }

    // request_id 파싱
    auto [ptr, ec] = std::from_chars(num_start, underscore, request_id);
    if (ec != std::errc{} || ptr != underscore) {
        return false;
    }

    // side 파싱
    const char* side = underscore + 1;
    if (std::strcmp(side, "buy") == 0) {
        is_buy_side = true;
    } else if (std::strcmp(side, "sell") == 0) {
        is_buy_side = false;
    } else {
        return false;
    }

    return true;
}

// =============================================================================
// 주문 등록 (DualOrderExecutor에서 호출)
// =============================================================================

void OrderTracker::register_order(const char* client_order_id,
                                   const char* exchange_order_id) {
    SpinLockGuard guard(lock_);

    int64_t request_id;
    bool is_buy_side;
    if (!parse_client_order_id(client_order_id, request_id, is_buy_side)) {
        logger_->warn("Invalid client_order_id format: {}", client_order_id);
        return;
    }

    // 이미 WS 업데이트로 생성된 슬롯이 있는지 확인 (역전 케이스)
    int idx = find_order(client_order_id);
    if (idx >= 0) {
        // WS가 먼저 도착한 경우 — registered 플래그만 설정
        orders_[idx].registered = true;
        if (exchange_order_id) {
            orders_[idx].last_update.set_order_id(exchange_order_id);
        }
        logger_->debug("register_order: WS already received for {}", client_order_id);
        return;
    }

    // 새 슬롯 할당
    idx = alloc_order();
    if (idx < 0) {
        logger_->error("OrderTracker full, cannot register {}", client_order_id);
        return;
    }

    auto& order = orders_[idx];
    order.active = true;
    order.registered = true;
    order.ws_received = false;
    order.request_id = request_id;
    order.is_buy_side = is_buy_side;
    order.created_at_ms = now_ms();
    order.last_update.set_client_order_id(client_order_id);
    if (exchange_order_id) {
        order.last_update.set_order_id(exchange_order_id);
    }

    // DualOrderTrack에 연결
    int dual_idx = find_or_alloc_dual(request_id);
    if (dual_idx >= 0) {
        auto& dual = duals_[dual_idx];
        if (is_buy_side) {
            dual.buy_idx = idx;
        } else {
            dual.sell_idx = idx;
        }
    }

    logger_->debug("register_order: {} (request_id={}, side={})",
                   client_order_id, request_id, is_buy_side ? "buy" : "sell");
}

// =============================================================================
// Private WS 업데이트 수신
// =============================================================================

void OrderTracker::on_order_update(const OrderUpdate& update) {
    if (!update.has_client_order_id()) {
        return;  // client_order_id 없으면 추적 불가
    }

    SpinLockGuard guard(lock_);

    int idx = find_order(update.client_order_id);
    if (idx < 0) {
        // REST 응답보다 WS가 먼저 도착한 경우 (역전) — 새 슬롯 생성
        int64_t request_id;
        bool is_buy_side;
        if (!parse_client_order_id(update.client_order_id, request_id, is_buy_side)) {
            return;  // 우리 주문이 아님
        }

        idx = alloc_order();
        if (idx < 0) {
            logger_->error("OrderTracker full, dropping WS update for {}",
                           update.client_order_id);
            return;
        }

        auto& order = orders_[idx];
        order.active = true;
        order.registered = false;  // REST 아직 안 옴
        order.request_id = request_id;
        order.is_buy_side = is_buy_side;
        order.created_at_ms = now_ms();

        // DualOrderTrack에 연결
        int dual_idx = find_or_alloc_dual(request_id);
        if (dual_idx >= 0) {
            auto& dual = duals_[dual_idx];
            if (is_buy_side) {
                dual.buy_idx = idx;
            } else {
                dual.sell_idx = idx;
            }
        }

        logger_->debug("on_order_update: WS arrived before REST for {}",
                       update.client_order_id);
    }

    // 상태 업데이트
    auto& order = orders_[idx];
    order.last_update = update;
    order.ws_received = true;

    logger_->debug("on_order_update: {} status={} filled={:.4f}",
                   update.client_order_id,
                   static_cast<int>(update.status),
                   update.filled_qty);

    // terminal 상태면 completion 체크
    if (update.is_terminal()) {
        // 이 주문이 속한 dual track 찾기
        for (size_t i = 0; i < MAX_ORDER_TRACKER_ENTRIES; ++i) {
            if (duals_[i].active && duals_[i].request_id == order.request_id) {
                check_completion(duals_[i]);
                break;
            }
        }
    }
}

// =============================================================================
// 내부 헬퍼
// =============================================================================

int OrderTracker::find_order(const char* client_order_id) const {
    for (size_t i = 0; i < MAX_ORDER_TRACKER_ENTRIES * 2; ++i) {
        if (orders_[i].active &&
            std::strcmp(orders_[i].last_update.client_order_id, client_order_id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int OrderTracker::alloc_order() {
    for (size_t i = 0; i < MAX_ORDER_TRACKER_ENTRIES * 2; ++i) {
        if (!orders_[i].active) {
            orders_[i].reset();
            return static_cast<int>(i);
        }
    }
    return -1;
}

int OrderTracker::find_or_alloc_dual(int64_t request_id) {
    // 기존 검색
    for (size_t i = 0; i < MAX_ORDER_TRACKER_ENTRIES; ++i) {
        if (duals_[i].active && duals_[i].request_id == request_id) {
            return static_cast<int>(i);
        }
    }
    // 새 할당
    for (size_t i = 0; i < MAX_ORDER_TRACKER_ENTRIES; ++i) {
        if (!duals_[i].active) {
            duals_[i].reset();
            duals_[i].active = true;
            duals_[i].request_id = request_id;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void OrderTracker::check_completion(DualOrderTrack& track) {
    if (track.completed) return;

    bool buy_done = false;
    bool sell_done = false;

    if (track.buy_idx >= 0) {
        const auto& buy = orders_[track.buy_idx];
        buy_done = buy.ws_received && buy.last_update.is_terminal();
    }

    if (track.sell_idx >= 0) {
        const auto& sell = orders_[track.sell_idx];
        sell_done = sell.ws_received && sell.last_update.is_terminal();
    }

    // 양쪽 모두 있고 terminal이면 완료
    if (track.buy_idx >= 0 && track.sell_idx >= 0 && buy_done && sell_done) {
        track.completed = true;

        logger_->info("Dual order completed: request_id={}, buy={}, sell={}",
                      track.request_id,
                      static_cast<int>(orders_[track.buy_idx].last_update.status),
                      static_cast<int>(orders_[track.sell_idx].last_update.status));

        if (on_complete_) {
            on_complete_(track.request_id,
                         orders_[track.buy_idx],
                         orders_[track.sell_idx]);
        }
    }
}

void OrderTracker::check_timeouts(int64_t ws_timeout_ms) {
    // 콜백은 lock 밖에서 호출 (콜백이 on_order_update 등으로 재진입 가능)
    // 한 번에 최대 16건 — 초과분은 다음 호출에서 처리 (escalated 플래그 미설정)
    constexpr size_t MAX_ESCALATIONS_PER_CALL = 16;
    TrackedOrder escalated[MAX_ESCALATIONS_PER_CALL];
    size_t escalated_count = 0;

    {
        SpinLockGuard guard(lock_);
        int64_t now = now_ms();

        for (size_t i = 0; i < MAX_ORDER_TRACKER_ENTRIES * 2 &&
                           escalated_count < MAX_ESCALATIONS_PER_CALL; ++i) {
            auto& order = orders_[i];
            if (!order.active || order.timeout_escalated) continue;
            if (order.ws_received && order.last_update.is_terminal()) continue;
            if ((now - order.created_at_ms) <= ws_timeout_ms) continue;

            order.timeout_escalated = true;
            escalated[escalated_count++] = order;

            logger_->warn("WS update timeout ({}ms): {} — escalating to REST fallback",
                          ws_timeout_ms, order.last_update.client_order_id);
        }
    }

    if (on_timeout_) {
        for (size_t i = 0; i < escalated_count; ++i) {
            on_timeout_(escalated[i]);
        }
    }
}

void OrderTracker::cleanup_stale(int64_t max_age_ms) {
    SpinLockGuard guard(lock_);
    int64_t now = now_ms();

    // dual과 양쪽 레그를 함께 해제 (dangling index 방지)
    auto release_dual = [this](DualOrderTrack& dual) {
        if (dual.buy_idx >= 0) orders_[dual.buy_idx].reset();
        if (dual.sell_idx >= 0) orders_[dual.sell_idx].reset();
        dual.reset();
    };

    auto is_stale = [&](int idx) {
        return idx >= 0 && orders_[idx].active &&
               (now - orders_[idx].created_at_ms) > max_age_ms;
    };

    for (size_t i = 0; i < MAX_ORDER_TRACKER_ENTRIES; ++i) {
        auto& dual = duals_[i];
        if (!dual.active) continue;

        if (dual.completed) {
            // 완료된 건 정리
            release_dual(dual);
        } else if (is_stale(dual.buy_idx) || is_stale(dual.sell_idx)) {
            // 미완료인데 오래된 건 — 헤징 상태 불명이므로 경고 후 정리
            logger_->warn("Stale incomplete dual order cleanup: request_id={} "
                          "(hedge state unknown, verify positions manually)",
                          dual.request_id);
            release_dual(dual);
        }
    }

    // dual에 연결되지 않은 고아 주문 정리
    for (size_t i = 0; i < MAX_ORDER_TRACKER_ENTRIES * 2; ++i) {
        if (orders_[i].active && (now - orders_[i].created_at_ms) > max_age_ms) {
            logger_->warn("Stale orphan order cleanup: {}",
                          orders_[i].last_update.client_order_id);
            // 혹시 남은 dual 참조가 있으면 함께 해제
            for (size_t d = 0; d < MAX_ORDER_TRACKER_ENTRIES; ++d) {
                if (duals_[d].active &&
                    (duals_[d].buy_idx == static_cast<int>(i) ||
                     duals_[d].sell_idx == static_cast<int>(i))) {
                    release_dual(duals_[d]);
                    break;
                }
            }
            orders_[i].reset();
        }
    }
}

size_t OrderTracker::active_count() const {
    SpinLockGuard guard(lock_);
    size_t count = 0;
    for (size_t i = 0; i < MAX_ORDER_TRACKER_ENTRIES * 2; ++i) {
        if (orders_[i].active) ++count;
    }
    return count;
}

const TrackedOrder* OrderTracker::get_order(const char* client_order_id) const {
    SpinLockGuard guard(lock_);
    int idx = find_order(client_order_id);
    return (idx >= 0) ? &orders_[idx] : nullptr;
}

int64_t OrderTracker::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace arbitrage
