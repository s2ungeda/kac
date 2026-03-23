#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <sys/types.h>
#include "arbitrage/common/types.hpp"

namespace arbitrage {

// =============================================================================
// SHM 상수
// =============================================================================

constexpr uint64_t SHM_MAGIC = 0xDEADBEEF4B494D43ULL;  // "KIMC" + DEADBEEF
constexpr uint64_t SHM_VERSION = 1;

// SHM 이름 규칙
namespace shm_names {
    constexpr const char* FEED_UPBIT    = "/kimchi_feed_upbit";
    constexpr const char* FEED_BITHUMB  = "/kimchi_feed_bithumb";
    constexpr const char* FEED_BINANCE  = "/kimchi_feed_binance";
    constexpr const char* FEED_MEXC     = "/kimchi_feed_mexc";
    constexpr const char* ORDERS        = "/kimchi_orders";
    constexpr const char* ORDER_RESULTS = "/kimchi_order_results";

    inline const char* feed_name(Exchange ex) {
        switch (ex) {
            case Exchange::Upbit:   return FEED_UPBIT;
            case Exchange::Bithumb: return FEED_BITHUMB;
            case Exchange::Binance: return FEED_BINANCE;
            case Exchange::MEXC:    return FEED_MEXC;
            default:                return nullptr;
        }
    }
}  // namespace shm_names

// =============================================================================
// SHM Queue State
// =============================================================================

enum class ShmQueueState : uint8_t {
    Init   = 0,   // 초기화 중
    Ready  = 1,   // 사용 가능
    Closed = 2    // 종료됨
};

// =============================================================================
// SHM Queue Header (64 bytes, cache-line aligned)
// =============================================================================
//
// Memory Layout:
//   Offset 0:    ShmQueueHeader  (64 bytes)
//   Offset 64:   head            (64 bytes, cache-line padded)
//   Offset 128:  tail            (64 bytes, cache-line padded)
//   Offset 192:  T buffer[capacity]
//
struct alignas(CACHE_LINE_SIZE) ShmQueueHeader {
    uint64_t magic;           // 유효성 검사 (SHM_MAGIC)
    uint64_t version;         // 프로토콜 버전
    uint64_t capacity;        // 링 버퍼 크기 (power of 2)
    uint64_t element_size;    // sizeof(T)
    pid_t    producer_pid;    // Writer PID
    pid_t    consumer_pid;    // Reader PID
    uint8_t  state;           // ShmQueueState
    uint8_t  _pad[64 - 8*4 - sizeof(pid_t)*2 - 1];
};
static_assert(sizeof(ShmQueueHeader) == CACHE_LINE_SIZE,
              "ShmQueueHeader must be 1 cache line");

// =============================================================================
// Cache-line padded atomic index
// =============================================================================

struct alignas(CACHE_LINE_SIZE) ShmAtomicIndex {
    std::atomic<uint64_t> value{0};
    char _pad[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(ShmAtomicIndex) == CACHE_LINE_SIZE,
              "ShmAtomicIndex must be 1 cache line");

// =============================================================================
// SHM 크기 계산
// =============================================================================

inline constexpr size_t shm_queue_size(size_t capacity, size_t element_size) {
    // header(64) + head(64) + tail(64) + buffer(capacity * element_size)
    return sizeof(ShmQueueHeader) + 2 * sizeof(ShmAtomicIndex)
           + capacity * element_size;
}

// =============================================================================
// TASK_43: SHM Order POD Types
// =============================================================================
// SingleOrderResult의 std::optional/std::string → 고정 크기 POD 변환

struct ShmSingleOrderResult {
    Exchange exchange;                        // 1 byte
    bool success;                             // 1 byte
    OrderStatus status;                       // 1 byte
    uint8_t _pad1[5];                         // 5 bytes

    // OrderResult 필드 (인라인)
    char order_id[MAX_ORDER_ID_LEN];          // 48 bytes
    double filled_qty;                        // 8 bytes
    double avg_price;                         // 8 bytes
    double commission;                        // 8 bytes

    // 시간 (microseconds since epoch — steady_clock 대신 system_clock)
    int64_t latency_us;                       // 8 bytes
    int64_t start_time_us;                    // 8 bytes
    int64_t end_time_us;                      // 8 bytes

    // 에러 (Error의 std::string → char[])
    uint16_t error_code;                      // 2 bytes
    uint8_t _pad2[6];                         // 6 bytes
    char error_message[MAX_MESSAGE_LEN];      // 128 bytes
};

static_assert(std::is_trivially_copyable_v<ShmSingleOrderResult>,
              "ShmSingleOrderResult must be trivially copyable");

struct alignas(CACHE_LINE_SIZE) ShmDualOrderResult {
    ShmSingleOrderResult buy_result;
    ShmSingleOrderResult sell_result;

    int64_t request_id;
    double actual_premium;
    double gross_profit;
    int64_t total_latency_us;

    bool both_success() const {
        return buy_result.success && sell_result.success;
    }

    bool both_filled() const {
        return buy_result.success && sell_result.success &&
               buy_result.status == OrderStatus::Filled &&
               sell_result.status == OrderStatus::Filled;
    }

    bool partial_fill() const {
        return (buy_result.success && !sell_result.success) ||
               (!buy_result.success && sell_result.success);
    }
};

static_assert(std::is_trivially_copyable_v<ShmDualOrderResult>,
              "ShmDualOrderResult must be trivially copyable");

}  // namespace arbitrage
