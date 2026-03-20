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
// ShmSPSCQueue<T> - Shared Memory Single Producer Single Consumer Queue
// =============================================================================
//
// 프로세스 간 Lock-Free SPSC Queue.
// T는 반드시 trivially copyable, 고정 크기여야 함.
//
// 메모리 레이아웃 (void* mem 기준):
//   [0..63]   ShmQueueHeader
//   [64..127] head (ShmAtomicIndex)
//   [128..191] tail (ShmAtomicIndex)
//   [192..]   T buffer[capacity]
//
// 사용법:
//   Producer: auto q = ShmSPSCQueue<Ticker>::init_producer(mem, 4096);
//   Consumer: auto q = ShmSPSCQueue<Ticker>::attach_consumer(mem);
//
template <typename T>
class ShmSPSCQueue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "ShmSPSCQueue requires trivially copyable type");

public:
    // =========================================================================
    // Factory methods
    // =========================================================================

    // Producer: 새 큐 초기화 (SHM 세그먼트의 시작 주소)
    static ShmSPSCQueue<T> init_producer(void* mem, size_t capacity) {
        // capacity는 2의 거듭제곱
        if ((capacity & (capacity - 1)) != 0 || capacity == 0) {
            return ShmSPSCQueue<T>();  // invalid
        }

        auto* header = static_cast<ShmQueueHeader*>(mem);
        header->magic = SHM_MAGIC;
        header->version = SHM_VERSION;
        header->capacity = capacity;
        header->element_size = sizeof(T);
        header->producer_pid = ::getpid();
        header->consumer_pid = 0;
        header->state = static_cast<uint8_t>(ShmQueueState::Init);

        // head/tail 초기화
        auto* head_idx = reinterpret_cast<ShmAtomicIndex*>(
            static_cast<char*>(mem) + sizeof(ShmQueueHeader));
        auto* tail_idx = reinterpret_cast<ShmAtomicIndex*>(
            static_cast<char*>(mem) + sizeof(ShmQueueHeader) + sizeof(ShmAtomicIndex));

        head_idx->value.store(0, std::memory_order_relaxed);
        tail_idx->value.store(0, std::memory_order_relaxed);

        // 버퍼 영역 zero 초기화
        auto* buf = static_cast<char*>(mem) + sizeof(ShmQueueHeader)
                    + 2 * sizeof(ShmAtomicIndex);
        std::memset(buf, 0, capacity * sizeof(T));

        // Ready 상태로 전환
        std::atomic_thread_fence(std::memory_order_release);
        header->state = static_cast<uint8_t>(ShmQueueState::Ready);

        return ShmSPSCQueue<T>(mem, true);
    }

    // Consumer: 기존 큐에 연결
    static ShmSPSCQueue<T> attach_consumer(void* mem) {
        auto* header = static_cast<ShmQueueHeader*>(mem);

        // 유효성 검사
        if (header->magic != SHM_MAGIC) {
            return ShmSPSCQueue<T>();  // invalid magic
        }
        if (header->version != SHM_VERSION) {
            return ShmSPSCQueue<T>();  // version mismatch
        }
        if (header->element_size != sizeof(T)) {
            return ShmSPSCQueue<T>();  // type size mismatch
        }
        if (header->state != static_cast<uint8_t>(ShmQueueState::Ready)) {
            return ShmSPSCQueue<T>();  // not ready
        }

        header->consumer_pid = ::getpid();
        std::atomic_thread_fence(std::memory_order_release);

        return ShmSPSCQueue<T>(mem, false);
    }

    // Default constructor (invalid state)
    ShmSPSCQueue() = default;

    // =========================================================================
    // Producer API
    // =========================================================================

    HOT_FUNCTION bool push(const T& item) {
        if (UNLIKELY(!valid_)) return false;

        const uint64_t head = head_->value.load(std::memory_order_relaxed);
        const uint64_t next = (head + 1) & mask_;

        if (UNLIKELY(next == tail_->value.load(std::memory_order_acquire))) {
            return false;  // Full
        }

        // trivially_copyable → memcpy로 안전하게 복사
        std::memcpy(&buffer_[head], &item, sizeof(T));
        head_->value.store(next, std::memory_order_release);
        return true;
    }

    // =========================================================================
    // Consumer API
    // =========================================================================

    HOT_FUNCTION bool pop(T& item) {
        if (UNLIKELY(!valid_)) return false;

        const uint64_t tail = tail_->value.load(std::memory_order_relaxed);

        if (UNLIKELY(tail == head_->value.load(std::memory_order_acquire))) {
            return false;  // Empty
        }

        std::memcpy(&item, &buffer_[tail], sizeof(T));
        tail_->value.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

    // =========================================================================
    // 상태 조회
    // =========================================================================

    bool empty() const {
        if (!valid_) return true;
        return head_->value.load(std::memory_order_acquire)
            == tail_->value.load(std::memory_order_acquire);
    }

    size_t size() const {
        if (!valid_) return 0;
        const uint64_t h = head_->value.load(std::memory_order_acquire);
        const uint64_t t = tail_->value.load(std::memory_order_acquire);
        return (h - t) & mask_;
    }

    size_t capacity() const {
        return valid_ ? header_->capacity : 0;
    }

    bool valid() const { return valid_; }

    // Producer가 close() 호출했는지 확인
    bool is_closed() const {
        if (!valid_) return true;
        return header_->state == static_cast<uint8_t>(ShmQueueState::Closed);
    }

    // Producer 프로세스가 살아있는지 확인 (kill(pid, 0))
    bool is_producer_alive() const {
        if (!valid_) return false;
        pid_t pid = header_->producer_pid;
        if (pid <= 0) return false;
        return ::kill(pid, 0) == 0;
    }

    // Consumer 프로세스가 살아있는지 확인
    bool is_consumer_alive() const {
        if (!valid_) return false;
        pid_t pid = header_->consumer_pid;
        if (pid <= 0) return false;
        return ::kill(pid, 0) == 0;
    }

    // Producer: 큐 종료 (state = Closed)
    void close() {
        if (!valid_) return;
        header_->state = static_cast<uint8_t>(ShmQueueState::Closed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    // Producer PID
    pid_t producer_pid() const {
        return valid_ ? header_->producer_pid : 0;
    }

    // Consumer PID
    pid_t consumer_pid() const {
        return valid_ ? header_->consumer_pid : 0;
    }

private:
    explicit ShmSPSCQueue(void* mem, bool is_producer)
        : valid_(true)
        , is_producer_(is_producer)
    {
        auto* base = static_cast<char*>(mem);
        header_ = reinterpret_cast<ShmQueueHeader*>(base);
        head_   = reinterpret_cast<ShmAtomicIndex*>(base + sizeof(ShmQueueHeader));
        tail_   = reinterpret_cast<ShmAtomicIndex*>(base + sizeof(ShmQueueHeader)
                                                         + sizeof(ShmAtomicIndex));
        buffer_ = reinterpret_cast<T*>(base + sizeof(ShmQueueHeader)
                                            + 2 * sizeof(ShmAtomicIndex));
        mask_   = header_->capacity - 1;
    }

    bool valid_{false};
    bool is_producer_{false};
    ShmQueueHeader* header_{nullptr};
    ShmAtomicIndex* head_{nullptr};
    ShmAtomicIndex* tail_{nullptr};
    T* buffer_{nullptr};
    uint64_t mask_{0};
};

// =============================================================================
// Static assertions
// =============================================================================
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "atomic<uint64_t> must be lock-free for SHM queue");

}  // namespace arbitrage
