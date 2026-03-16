#pragma once

/**
 * Event Types (TASK_19)
 *
 * 시스템 전역 이벤트 정의
 * - 시세 이벤트
 * - 김프 이벤트
 * - 주문 이벤트
 * - 시스템 이벤트
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/crypto.hpp"

#include <variant>
#include <chrono>
#include <string>

namespace arbitrage::events {

// =============================================================================
// 기본 이벤트
// =============================================================================
struct EventBase {
    std::string id;
    std::chrono::system_clock::time_point timestamp;

    EventBase()
        : id(generate_uuid())
        , timestamp(std::chrono::system_clock::now())
    {}
};

// =============================================================================
// 시세 이벤트
// =============================================================================

/**
 * 시세 수신
 */
struct TickerReceived : EventBase {
    Ticker ticker;

    TickerReceived() = default;
    explicit TickerReceived(const Ticker& t) : ticker(t) {}
};

/**
 * 오더북 업데이트
 */
struct OrderBookUpdated : EventBase {
    Exchange exchange;
    OrderBook orderbook;

    OrderBookUpdated() = default;
    OrderBookUpdated(Exchange ex, const OrderBook& ob)
        : exchange(ex), orderbook(ob) {}
};

// =============================================================================
// 김프 이벤트
// =============================================================================

/**
 * 프리미엄 업데이트
 */
struct PremiumUpdated : EventBase {
    double premium_pct{0.0};
    Exchange buy_exchange;
    Exchange sell_exchange;
    double buy_price{0.0};
    double sell_price{0.0};

    PremiumUpdated() = default;
    PremiumUpdated(double pct, Exchange buy_ex, Exchange sell_ex)
        : premium_pct(pct)
        , buy_exchange(buy_ex)
        , sell_exchange(sell_ex)
    {}
};

/**
 * 기회 감지
 */
struct OpportunityDetected : EventBase {
    double premium_pct{0.0};
    Exchange buy_exchange;
    Exchange sell_exchange;
    double recommended_qty{0.0};
    double expected_profit{0.0};
    double confidence{0.0};

    OpportunityDetected() = default;
};

// =============================================================================
// 주문 이벤트
// =============================================================================

/**
 * 주문 제출됨
 */
struct OrderSubmitted : EventBase {
    std::string order_id;
    Exchange exchange;
    OrderSide side;
    double quantity{0.0};
    double price{0.0};

    OrderSubmitted() = default;
};

/**
 * 주문 체결됨
 */
struct OrderFilled : EventBase {
    std::string order_id;
    Exchange exchange;
    double filled_qty{0.0};
    double avg_price{0.0};
    double fee{0.0};

    OrderFilled() = default;
};

/**
 * 주문 부분 체결
 */
struct OrderPartialFilled : EventBase {
    std::string order_id;
    Exchange exchange;
    double filled_qty{0.0};
    double remaining_qty{0.0};
    double avg_price{0.0};

    OrderPartialFilled() = default;
};

/**
 * 주문 취소됨
 */
struct OrderCanceled : EventBase {
    std::string order_id;
    Exchange exchange;
    std::string reason;

    OrderCanceled() = default;
};

/**
 * 주문 실패
 */
struct OrderFailed : EventBase {
    std::string order_id;
    Exchange exchange;
    std::string error;
    int error_code{0};

    OrderFailed() = default;
};

// =============================================================================
// 듀얼 주문 이벤트
// =============================================================================

/**
 * 듀얼 주문 시작
 */
struct DualOrderStarted : EventBase {
    std::string request_id;
    Exchange buy_exchange;
    Exchange sell_exchange;
    double quantity{0.0};

    DualOrderStarted() = default;
};

/**
 * 듀얼 주문 완료
 */
struct DualOrderCompleted : EventBase {
    std::string request_id;
    bool success{false};
    double actual_profit{0.0};

    DualOrderCompleted() = default;
};

// =============================================================================
// 송금 이벤트
// =============================================================================

/**
 * 송금 시작
 */
struct TransferStarted : EventBase {
    std::string transfer_id;
    Exchange from_exchange;
    Exchange to_exchange;
    double amount{0.0};

    TransferStarted() = default;
};

/**
 * 송금 완료
 */
struct TransferCompleted : EventBase {
    std::string transfer_id;
    std::string tx_hash;
    std::chrono::milliseconds elapsed{0};

    TransferCompleted() = default;
};

/**
 * 송금 실패
 */
struct TransferFailed : EventBase {
    std::string transfer_id;
    std::string error;

    TransferFailed() = default;
};

// =============================================================================
// 시스템 이벤트
// =============================================================================

/**
 * 거래소 연결됨
 */
struct ExchangeConnected : EventBase {
    Exchange exchange;

    ExchangeConnected() = default;
    explicit ExchangeConnected(Exchange ex) : exchange(ex) {}
};

/**
 * 거래소 연결 해제됨
 */
struct ExchangeDisconnected : EventBase {
    Exchange exchange;
    std::string reason;
    bool will_reconnect{true};

    ExchangeDisconnected() = default;
    ExchangeDisconnected(Exchange ex, const std::string& r)
        : exchange(ex), reason(r) {}
};

/**
 * 킬 스위치 활성화
 */
struct KillSwitchActivated : EventBase {
    std::string reason;
    bool manual{false};

    KillSwitchActivated() = default;
    explicit KillSwitchActivated(const std::string& r, bool m = false)
        : reason(r), manual(m) {}
};

/**
 * 킬 스위치 해제
 */
struct KillSwitchDeactivated : EventBase {
    KillSwitchDeactivated() = default;
};

/**
 * 설정 리로드됨
 */
struct ConfigReloaded : EventBase {
    std::string config_path;
    bool success{true};
    std::string error;

    ConfigReloaded() = default;
};

/**
 * 일일 손실 한도 도달
 */
struct DailyLossLimitReached : EventBase {
    double loss_amount{0.0};
    double limit{0.0};

    DailyLossLimitReached() = default;
};

/**
 * 시스템 시작
 */
struct SystemStarted : EventBase {
    SystemStarted() = default;
};

/**
 * 시스템 종료
 */
struct SystemShutdown : EventBase {
    std::string reason;
    bool graceful{true};

    SystemShutdown() = default;
};

// =============================================================================
// 워치독 이벤트 (TASK_26)
// =============================================================================

/**
 * 하트비트 수신
 */
struct HeartbeatReceived : EventBase {
    uint64_t sequence{0};
    uint8_t component_status{0};

    HeartbeatReceived() = default;
};

/**
 * 프로세스 재시작
 */
struct ProcessRestarted : EventBase {
    int old_pid{0};
    int new_pid{0};
    std::string reason;

    ProcessRestarted() = default;
};

/**
 * 워치독 알림
 */
struct WatchdogAlert : EventBase {
    std::string level;
    std::string message;

    WatchdogAlert() = default;
};

// =============================================================================
// 이벤트 통합 타입
// =============================================================================
using Event = std::variant<
    // 시세
    TickerReceived,
    OrderBookUpdated,

    // 김프
    PremiumUpdated,
    OpportunityDetected,

    // 주문
    OrderSubmitted,
    OrderFilled,
    OrderPartialFilled,
    OrderCanceled,
    OrderFailed,

    // 듀얼 주문
    DualOrderStarted,
    DualOrderCompleted,

    // 송금
    TransferStarted,
    TransferCompleted,
    TransferFailed,

    // 시스템
    ExchangeConnected,
    ExchangeDisconnected,
    KillSwitchActivated,
    KillSwitchDeactivated,
    ConfigReloaded,
    DailyLossLimitReached,
    SystemStarted,
    SystemShutdown,

    // 워치독
    HeartbeatReceived,
    ProcessRestarted,
    WatchdogAlert
>;

// =============================================================================
// 이벤트 유틸리티
// =============================================================================

/**
 * 이벤트 ID 조회
 */
inline std::string get_event_id(const Event& event) {
    return std::visit([](const auto& e) { return e.id; }, event);
}

/**
 * 이벤트 타임스탬프 조회
 */
inline std::chrono::system_clock::time_point get_event_timestamp(const Event& event) {
    return std::visit([](const auto& e) { return e.timestamp; }, event);
}

/**
 * 이벤트 타입 이름
 */
inline const char* get_event_type_name(const Event& event) {
    return std::visit([](const auto& e) -> const char* {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, TickerReceived>) return "TickerReceived";
        else if constexpr (std::is_same_v<T, OrderBookUpdated>) return "OrderBookUpdated";
        else if constexpr (std::is_same_v<T, PremiumUpdated>) return "PremiumUpdated";
        else if constexpr (std::is_same_v<T, OpportunityDetected>) return "OpportunityDetected";
        else if constexpr (std::is_same_v<T, OrderSubmitted>) return "OrderSubmitted";
        else if constexpr (std::is_same_v<T, OrderFilled>) return "OrderFilled";
        else if constexpr (std::is_same_v<T, OrderPartialFilled>) return "OrderPartialFilled";
        else if constexpr (std::is_same_v<T, OrderCanceled>) return "OrderCanceled";
        else if constexpr (std::is_same_v<T, OrderFailed>) return "OrderFailed";
        else if constexpr (std::is_same_v<T, DualOrderStarted>) return "DualOrderStarted";
        else if constexpr (std::is_same_v<T, DualOrderCompleted>) return "DualOrderCompleted";
        else if constexpr (std::is_same_v<T, TransferStarted>) return "TransferStarted";
        else if constexpr (std::is_same_v<T, TransferCompleted>) return "TransferCompleted";
        else if constexpr (std::is_same_v<T, TransferFailed>) return "TransferFailed";
        else if constexpr (std::is_same_v<T, ExchangeConnected>) return "ExchangeConnected";
        else if constexpr (std::is_same_v<T, ExchangeDisconnected>) return "ExchangeDisconnected";
        else if constexpr (std::is_same_v<T, KillSwitchActivated>) return "KillSwitchActivated";
        else if constexpr (std::is_same_v<T, KillSwitchDeactivated>) return "KillSwitchDeactivated";
        else if constexpr (std::is_same_v<T, ConfigReloaded>) return "ConfigReloaded";
        else if constexpr (std::is_same_v<T, DailyLossLimitReached>) return "DailyLossLimitReached";
        else if constexpr (std::is_same_v<T, SystemStarted>) return "SystemStarted";
        else if constexpr (std::is_same_v<T, SystemShutdown>) return "SystemShutdown";
        else if constexpr (std::is_same_v<T, HeartbeatReceived>) return "HeartbeatReceived";
        else if constexpr (std::is_same_v<T, ProcessRestarted>) return "ProcessRestarted";
        else if constexpr (std::is_same_v<T, WatchdogAlert>) return "WatchdogAlert";
        else return "Unknown";
    }, event);
}

}  // namespace arbitrage::events
