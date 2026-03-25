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
// ShmRingBuffer<T> - 덮어쓰기 링버퍼 (오래된 데이터 자동 폐기)
// =============================================================================
//
// Ticker처럼 "최신 흐름"이 중요한 데이터에 사용.
// Producer는 항상 성공 (큐 full이면 오래된 데이터를 덮어씀).
// Consumer가 느리면 자동으로 최신 위치로 catch-up.
//
// 메모리 레이아웃: ShmSPSCQueue와 동일
//   [0..63]     ShmQueueHeader
//   [64..127]   head (ShmAtomicIndex) — 단조 증가, Producer만 쓰기
//   [128..191]  tail (ShmAtomicIndex) — 단조 증가, Consumer만 쓰기
//   [192..]     T buffer[capacity]
//
// head/tail은 wrap하지 않음 (uint64_t 단조 증가).
// 인덱싱: buffer[pos & mask_]
//
template <typename T>
class ShmRingBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "ShmRingBuffer requires trivially copyable type");

public:
    // =========================================================================
    // Factory methods
    // =========================================================================

    static ShmRingBuffer<T> init_producer(void* mem, size_t capacity) {
        if ((capacity & (capacity - 1)) != 0 || capacity == 0) {
            return ShmRingBuffer<T>();
        }

        auto* header = static_cast<ShmQueueHeader*>(mem);
        header->magic = SHM_MAGIC;
        header->version = SHM_VERSION;
        header->capacity = capacity;
        header->element_size = sizeof(T);
        header->producer_pid = ::getpid();
        header->consumer_pid = 0;
        header->state = static_cast<uint8_t>(ShmQueueState::Init);

        auto* head_idx = reinterpret_cast<ShmAtomicIndex*>(
            static_cast<char*>(mem) + sizeof(ShmQueueHeader));
        auto* tail_idx = reinterpret_cast<ShmAtomicIndex*>(
            static_cast<char*>(mem) + sizeof(ShmQueueHeader) + sizeof(ShmAtomicIndex));

        head_idx->value.store(0, std::memory_order_relaxed);
        tail_idx->value.store(0, std::memory_order_relaxed);

        auto* buf = static_cast<char*>(mem) + sizeof(ShmQueueHeader)
                    + 2 * sizeof(ShmAtomicIndex);
        std::memset(buf, 0, capacity * sizeof(T));

        std::atomic_thread_fence(std::memory_order_release);
        header->state = static_cast<uint8_t>(ShmQueueState::Ready);

        return ShmRingBuffer<T>(mem);
    }

    static ShmRingBuffer<T> attach_consumer(void* mem) {
        auto* header = static_cast<ShmQueueHeader*>(mem);

        if (header->magic != SHM_MAGIC) return ShmRingBuffer<T>();
        if (header->version != SHM_VERSION) return ShmRingBuffer<T>();
        if (header->element_size != sizeof(T)) return ShmRingBuffer<T>();
        if (header->state != static_cast<uint8_t>(ShmQueueState::Ready))
            return ShmRingBuffer<T>();

        header->consumer_pid = ::getpid();
        std::atomic_thread_fence(std::memory_order_release);

        return ShmRingBuffer<T>(mem);
    }

    ShmRingBuffer() = default;

    // =========================================================================
    // Producer API — 항상 성공, 오래된 데이터 덮어씀
    // =========================================================================

    HOT_FUNCTION void push(const T& item) {
        if (UNLIKELY(!valid_)) return;

        const uint64_t h = head_->value.load(std::memory_order_relaxed);
        std::memcpy(&buffer_[h & mask_], &item, sizeof(T));
        head_->value.store(h + 1, std::memory_order_release);
    }

    // =========================================================================
    // Consumer API — 비었으면 false, 뒤처졌으면 catch-up
    // =========================================================================

    HOT_FUNCTION bool pop(T& item) {
        if (UNLIKELY(!valid_)) return false;

        uint64_t t = tail_->value.load(std::memory_order_relaxed);
        const uint64_t h = head_->value.load(std::memory_order_acquire);

        if (t >= h) return false;  // 비어있음

        // Consumer가 뒤처졌으면 최신 위치로 catch-up
        // (capacity-1) 간격 유지 — Producer와 같은 슬롯 접근 방지
        if (h - t > mask_) {
            t = h - mask_;
        }

        std::memcpy(&item, &buffer_[t & mask_], sizeof(T));
        tail_->value.store(t + 1, std::memory_order_release);
        return true;
    }

    // 큐에 남아있는 모든 항목을 drain하고 마지막 것만 반환
    HOT_FUNCTION bool pop_latest(T& item) {
        if (UNLIKELY(!valid_)) return false;

        const uint64_t h = head_->value.load(std::memory_order_acquire);
        uint64_t t = tail_->value.load(std::memory_order_relaxed);

        if (t >= h) return false;

        // 마지막 항목으로 바로 이동
        t = h - 1;
        std::memcpy(&item, &buffer_[t & mask_], sizeof(T));
        tail_->value.store(h, std::memory_order_release);
        return true;
    }

    // =========================================================================
    // 상태 조회
    // =========================================================================

    bool empty() const {
        if (!valid_) return true;
        return head_->value.load(std::memory_order_acquire)
            <= tail_->value.load(std::memory_order_acquire);
    }

    size_t size() const {
        if (!valid_) return 0;
        const uint64_t h = head_->value.load(std::memory_order_acquire);
        const uint64_t t = tail_->value.load(std::memory_order_acquire);
        if (h <= t) return 0;
        uint64_t diff = h - t;
        return (diff > capacity_) ? capacity_ : static_cast<size_t>(diff);
    }

    size_t capacity() const { return valid_ ? capacity_ : 0; }
    bool valid() const { return valid_; }

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

    pid_t producer_pid() const { return valid_ ? header_->producer_pid : 0; }
    pid_t consumer_pid() const { return valid_ ? header_->consumer_pid : 0; }

    // 총 push된 수 (단조 증가 head값)
    uint64_t total_pushed() const {
        return valid_ ? head_->value.load(std::memory_order_relaxed) : 0;
    }

private:
    explicit ShmRingBuffer(void* mem)
        : valid_(true)
    {
        auto* base = static_cast<char*>(mem);
        header_   = reinterpret_cast<ShmQueueHeader*>(base);
        head_     = reinterpret_cast<ShmAtomicIndex*>(base + sizeof(ShmQueueHeader));
        tail_     = reinterpret_cast<ShmAtomicIndex*>(base + sizeof(ShmQueueHeader)
                                                           + sizeof(ShmAtomicIndex));
        buffer_   = reinterpret_cast<T*>(base + sizeof(ShmQueueHeader)
                                              + 2 * sizeof(ShmAtomicIndex));
        capacity_ = header_->capacity;
        mask_     = capacity_ - 1;
    }

    bool valid_{false};
    ShmQueueHeader* header_{nullptr};
    ShmAtomicIndex* head_{nullptr};
    ShmAtomicIndex* tail_{nullptr};
    T* buffer_{nullptr};
    uint64_t capacity_{0};
    uint64_t mask_{0};
};

}  // namespace arbitrage
