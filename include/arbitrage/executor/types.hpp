#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/error.hpp"
#include <optional>
#include <chrono>
#include <atomic>
#include <string>

namespace arbitrage {

// =============================================================================
// 동시 주문 실행기 타입 정의
// =============================================================================

// 복구 액션 타입
enum class RecoveryAction : uint8_t {
    None,              // 복구 불필요 (둘 다 성공 또는 둘 다 실패)
    SellBought,        // 매수한 포지션 손절 매도 (해외에서 매수 성공, 국내 매도 실패)
    BuySold,           // 매도한 포지션 매수 복구 (국내에서 매도 성공, 해외 매수 실패)
    CancelBoth,        // 둘 다 취소 시도
    ManualIntervention // 수동 개입 필요
};

// 복구 액션 이름 변환
constexpr const char* recovery_action_name(RecoveryAction action) {
    switch (action) {
        case RecoveryAction::None:              return "None";
        case RecoveryAction::SellBought:        return "SellBought";
        case RecoveryAction::BuySold:           return "BuySold";
        case RecoveryAction::CancelBoth:        return "CancelBoth";
        case RecoveryAction::ManualIntervention: return "ManualIntervention";
        default:                                return "Unknown";
    }
}

// =============================================================================
// 동시 주문 요청 (Cache-line aligned)
// =============================================================================
struct alignas(CACHE_LINE_SIZE) DualOrderRequest {
    OrderRequest buy_order;      // 매수 주문 (해외 거래소)
    OrderRequest sell_order;     // 매도 주문 (국내 거래소)
    double expected_premium;     // 예상 프리미엄 (%)
    Duration buy_delay{0};       // RTT 보정 지연 (매수)
    Duration sell_delay{0};      // RTT 보정 지연 (매도)
    int64_t request_id{0};       // 요청 ID

    // 헬퍼 함수
    void set_request_id_auto() {
        auto now = std::chrono::steady_clock::now();
        request_id = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

    bool is_valid() const {
        // 매수와 매도가 서로 다른 거래소여야 함
        if (buy_order.exchange == sell_order.exchange) return false;
        // 매수는 Buy, 매도는 Sell이어야 함
        if (buy_order.side != OrderSide::Buy) return false;
        if (sell_order.side != OrderSide::Sell) return false;
        // 수량이 양수여야 함
        if (buy_order.quantity <= 0 || sell_order.quantity <= 0) return false;
        return true;
    }
};

// =============================================================================
// 개별 주문 결과
// =============================================================================
struct SingleOrderResult {
    Exchange exchange;
    std::optional<OrderResult> result;
    std::optional<Error> error;
    Duration latency{0};          // 주문 실행 시간
    SteadyTimePoint start_time;   // 주문 시작 시간
    SteadyTimePoint end_time;     // 주문 완료 시간

    bool is_success() const {
        return result.has_value() && !error.has_value();
    }

    bool is_filled() const {
        return is_success() && result->is_filled();
    }

    bool is_partially_filled() const {
        return is_success() &&
               result->status == OrderStatus::PartiallyFilled &&
               result->filled_qty > 0;
    }

    bool is_failed() const {
        return error.has_value() ||
               (result.has_value() && result->is_failed());
    }

    double filled_qty() const {
        return result.has_value() ? result->filled_qty : 0.0;
    }

    double avg_price() const {
        return result.has_value() ? result->avg_price : 0.0;
    }

    const char* order_id() const {
        return result.has_value() ? result->order_id : "";
    }

    std::string error_message() const {
        if (error.has_value()) {
            return error->message;
        }
        if (result.has_value() && result->message[0] != '\0') {
            return std::string(result->message);
        }
        return "";
    }
};

// =============================================================================
// 동시 주문 결과 (Cache-line aligned)
// =============================================================================
struct alignas(CACHE_LINE_SIZE) DualOrderResult {
    SingleOrderResult buy_result;     // 매수 결과
    SingleOrderResult sell_result;    // 매도 결과
    SteadyTimePoint start_time;       // 전체 시작 시간
    SteadyTimePoint end_time;         // 전체 완료 시간
    int64_t request_id{0};            // 요청 ID (DualOrderRequest와 매칭)
    double actual_premium{0.0};       // 실제 체결 프리미엄 (%)

    // 상태 확인
    bool both_success() const {
        return buy_result.is_success() && sell_result.is_success();
    }

    bool both_filled() const {
        return buy_result.is_filled() && sell_result.is_filled();
    }

    bool both_failed() const {
        return buy_result.is_failed() && sell_result.is_failed();
    }

    bool partial_fill() const {
        // 하나만 성공한 경우 (복구 필요)
        return (buy_result.is_success() && sell_result.is_failed()) ||
               (buy_result.is_failed() && sell_result.is_success());
    }

    bool any_success() const {
        return buy_result.is_success() || sell_result.is_success();
    }

    // 지연 시간
    Duration total_latency() const {
        return std::chrono::duration_cast<Duration>(end_time - start_time);
    }

    Duration max_order_latency() const {
        return std::max(buy_result.latency, sell_result.latency);
    }

    // 총 체결 금액 계산 (KRW 기준, fx_rate 필요)
    double total_buy_cost(double fx_rate = 1.0) const {
        double cost = buy_result.filled_qty() * buy_result.avg_price();
        // 해외 거래소면 환율 적용
        if (!is_krw_exchange(buy_result.exchange)) {
            cost *= fx_rate;
        }
        return cost;
    }

    double total_sell_revenue(double fx_rate = 1.0) const {
        double revenue = sell_result.filled_qty() * sell_result.avg_price();
        // 해외 거래소면 환율 적용
        if (!is_krw_exchange(sell_result.exchange)) {
            revenue *= fx_rate;
        }
        return revenue;
    }

    // 예상 순익 (수수료 제외)
    double gross_profit(double fx_rate = 1.0) const {
        return total_sell_revenue(fx_rate) - total_buy_cost(fx_rate);
    }

    // 실제 프리미엄 계산 (체결 기준)
    void calculate_actual_premium(double fx_rate) {
        if (buy_result.avg_price() > 0 && sell_result.avg_price() > 0) {
            double buy_krw = buy_result.avg_price();
            double sell_krw = sell_result.avg_price();

            // 해외 거래소 가격을 KRW로 변환
            if (!is_krw_exchange(buy_result.exchange)) {
                buy_krw *= fx_rate;
            }
            if (!is_krw_exchange(sell_result.exchange)) {
                sell_krw *= fx_rate;
            }

            // 프리미엄 = (국내 - 해외) / 해외 * 100
            if (is_krw_exchange(sell_result.exchange)) {
                // 정방향: 해외 매수, 국내 매도
                actual_premium = (sell_krw - buy_krw) / buy_krw * 100.0;
            } else {
                // 역방향: 국내 매수, 해외 매도
                actual_premium = (buy_krw - sell_krw) / sell_krw * 100.0;
            }
        }
    }
};

// =============================================================================
// 복구 계획
// =============================================================================
struct RecoveryPlan {
    RecoveryAction action{RecoveryAction::None};
    OrderRequest recovery_order;     // 복구 주문
    std::string reason;              // 복구 사유
    int retry_count{0};              // 재시도 횟수
    int max_retries{3};              // 최대 재시도
    Duration retry_delay{100000};    // 재시도 간격 (100ms)

    bool needs_execution() const {
        return action != RecoveryAction::None &&
               action != RecoveryAction::ManualIntervention;
    }

    bool can_retry() const {
        return retry_count < max_retries;
    }

    void increment_retry() {
        ++retry_count;
    }
};

// =============================================================================
// 복구 결과
// =============================================================================
struct RecoveryResult {
    RecoveryPlan plan;
    SingleOrderResult result;
    bool success{false};
    std::string message;

    bool completed() const {
        return success || plan.retry_count >= plan.max_retries;
    }
};

// =============================================================================
// 실행기 통계
// =============================================================================
struct alignas(CACHE_LINE_SIZE) ExecutorStats {
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> successful_dual{0};      // 양쪽 모두 성공
    std::atomic<uint64_t> partial_success{0};      // 한쪽만 성공
    std::atomic<uint64_t> total_failures{0};       // 양쪽 모두 실패
    std::atomic<uint64_t> recovery_attempts{0};    // 복구 시도
    std::atomic<uint64_t> recovery_success{0};     // 복구 성공
    std::atomic<int64_t> total_latency_us{0};      // 총 지연 시간 (us)
    std::atomic<int64_t> max_latency_us{0};        // 최대 지연 시간
    std::atomic<int64_t> min_latency_us{INT64_MAX};// 최소 지연 시간

    void record_result(const DualOrderResult& result) {
        ++total_requests;

        if (result.both_success()) {
            ++successful_dual;
        } else if (result.partial_fill()) {
            ++partial_success;
        } else {
            ++total_failures;
        }

        auto latency_us = result.total_latency().count();
        total_latency_us.fetch_add(latency_us, std::memory_order_relaxed);

        // 최대/최소 업데이트 (CAS loop)
        int64_t current_max = max_latency_us.load(std::memory_order_relaxed);
        while (latency_us > current_max &&
               !max_latency_us.compare_exchange_weak(current_max, latency_us));

        int64_t current_min = min_latency_us.load(std::memory_order_relaxed);
        while (latency_us < current_min &&
               !min_latency_us.compare_exchange_weak(current_min, latency_us));
    }

    void record_recovery(bool success) {
        ++recovery_attempts;
        if (success) ++recovery_success;
    }

    double avg_latency_us() const {
        uint64_t total = total_requests.load();
        return total > 0 ?
            static_cast<double>(total_latency_us.load()) / total : 0.0;
    }

    double success_rate() const {
        uint64_t total = total_requests.load();
        return total > 0 ?
            static_cast<double>(successful_dual.load()) / total * 100.0 : 0.0;
    }

    double recovery_rate() const {
        uint64_t attempts = recovery_attempts.load();
        return attempts > 0 ?
            static_cast<double>(recovery_success.load()) / attempts * 100.0 : 0.0;
    }

    void reset() {
        total_requests = 0;
        successful_dual = 0;
        partial_success = 0;
        total_failures = 0;
        recovery_attempts = 0;
        recovery_success = 0;
        total_latency_us = 0;
        max_latency_us = 0;
        min_latency_us = INT64_MAX;
    }
};

}  // namespace arbitrage
