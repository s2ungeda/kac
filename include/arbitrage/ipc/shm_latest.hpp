#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <signal.h>
#include <unistd.h>

#include "arbitrage/ipc/ipc_types.hpp"
#include "arbitrage/common/compiler.hpp"

namespace arbitrage {

// =============================================================================
// ShmLatestValue<T> - 최신 값만 유지하는 SHM 슬롯 (Seqlock 기반)
// =============================================================================
//
// OrderBook처럼 "현재 상태"만 중요한 데이터에 사용.
// Producer가 새 값을 쓰면 이전 값은 덮어씌워짐.
//
// 메모리 레이아웃:
//   [0..63]     ShmQueueHeader (magic, version, pid)
//   [64..127]   sequence (ShmAtomicIndex) — seqlock 카운터
//   [128..]     T value
//
// Seqlock 프로토콜:
//   Writer: seq++ (홀수=쓰기중), memcpy, seq++ (짝수=완료)
//   Reader: seq1 읽기(짝수 확인), memcpy, seq2 읽기(seq1==seq2 확인)
//
template <typename T>
class ShmLatestValue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "ShmLatestValue requires trivially copyable type");

public:
    // =========================================================================
    // Factory methods
    // =========================================================================

    static ShmLatestValue<T> init_producer(void* mem, size_t /* ignored */) {
        auto* header = static_cast<ShmQueueHeader*>(mem);
        header->magic = SHM_MAGIC;
        header->version = SHM_VERSION;
        header->capacity = 1;  // 단일 슬롯
        header->element_size = sizeof(T);
        header->producer_pid = ::getpid();
        header->consumer_pid = 0;
        header->state = static_cast<uint8_t>(ShmQueueState::Init);

        auto* seq = reinterpret_cast<ShmAtomicIndex*>(
            static_cast<char*>(mem) + sizeof(ShmQueueHeader));
        seq->value.store(0, std::memory_order_relaxed);

        auto* value = reinterpret_cast<T*>(
            static_cast<char*>(mem) + sizeof(ShmQueueHeader) + sizeof(ShmAtomicIndex));
        std::memset(value, 0, sizeof(T));

        std::atomic_thread_fence(std::memory_order_release);
        header->state = static_cast<uint8_t>(ShmQueueState::Ready);

        return ShmLatestValue<T>(mem, true);
    }

    static ShmLatestValue<T> attach_consumer(void* mem) {
        auto* header = static_cast<ShmQueueHeader*>(mem);

        if (header->magic != SHM_MAGIC) return ShmLatestValue<T>();
        if (header->version != SHM_VERSION) return ShmLatestValue<T>();
        if (header->element_size != sizeof(T)) return ShmLatestValue<T>();
        if (header->state != static_cast<uint8_t>(ShmQueueState::Ready))
            return ShmLatestValue<T>();

        header->consumer_pid = ::getpid();
        std::atomic_thread_fence(std::memory_order_release);

        return ShmLatestValue<T>(mem, false);
    }

    ShmLatestValue() = default;

    // =========================================================================
    // Producer API — 항상 성공, 이전 값 덮어씀
    // =========================================================================

    HOT_FUNCTION void store(const T& item) {
        if (UNLIKELY(!valid_)) return;

        // 홀수 = 쓰기 시작
        uint64_t seq = seq_->value.load(std::memory_order_relaxed);
        seq_->value.store(seq + 1, std::memory_order_release);

        std::memcpy(value_, &item, sizeof(T));

        // 짝수 = 쓰기 완료
        seq_->value.store(seq + 2, std::memory_order_release);
    }

    // =========================================================================
    // Consumer API — 최신 값 읽기 (쓰기 중이면 false)
    // =========================================================================

    HOT_FUNCTION bool load(T& item) const {
        if (UNLIKELY(!valid_)) return false;

        uint64_t seq1 = seq_->value.load(std::memory_order_acquire);
        if (seq1 == 0) return false;  // 아직 한 번도 안 씀
        if (seq1 & 1) return false;   // 쓰기 중

        std::memcpy(&item, value_, sizeof(T));

        uint64_t seq2 = seq_->value.load(std::memory_order_acquire);
        return seq1 == seq2;  // 읽는 동안 변경 안 됨
    }

    // 스핀하며 읽기 (반드시 성공할 때까지)
    HOT_FUNCTION bool load_spin(T& item, int max_retries = 64) const {
        for (int i = 0; i < max_retries; ++i) {
            if (load(item)) return true;
        }
        return false;
    }

    // =========================================================================
    // 상태 조회
    // =========================================================================

    bool valid() const { return valid_; }

    uint64_t sequence() const {
        return valid_ ? seq_->value.load(std::memory_order_acquire) : 0;
    }

    bool has_data() const {
        return valid_ && seq_->value.load(std::memory_order_acquire) >= 2;
    }

    bool is_closed() const {
        if (!valid_) return true;
        return header_->state == static_cast<uint8_t>(ShmQueueState::Closed);
    }

    bool is_producer_alive() const {
        if (!valid_) return false;
        pid_t pid = header_->producer_pid;
        if (pid <= 0) return false;
        return ::kill(pid, 0) == 0;
    }

    void close() {
        if (!valid_) return;
        header_->state = static_cast<uint8_t>(ShmQueueState::Closed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    pid_t producer_pid() const {
        return valid_ ? header_->producer_pid : 0;
    }

    size_t capacity() const { return 1; }

private:
    explicit ShmLatestValue(void* mem, bool /*is_producer*/)
        : valid_(true)
    {
        auto* base = static_cast<char*>(mem);
        header_ = reinterpret_cast<ShmQueueHeader*>(base);
        seq_    = reinterpret_cast<ShmAtomicIndex*>(base + sizeof(ShmQueueHeader));
        value_  = reinterpret_cast<T*>(base + sizeof(ShmQueueHeader) + sizeof(ShmAtomicIndex));
    }

    bool valid_{false};
    ShmQueueHeader* header_{nullptr};
    ShmAtomicIndex* seq_{nullptr};
    T* value_{nullptr};
};

// SHM 크기 계산
template <typename T>
inline constexpr size_t shm_latest_size() {
    return sizeof(ShmQueueHeader) + sizeof(ShmAtomicIndex) + sizeof(T);
}

}  // namespace arbitrage
