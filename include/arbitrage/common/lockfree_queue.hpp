#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include "arbitrage/common/types.hpp"  // For CACHE_LINE_SIZE

namespace arbitrage {

// =============================================================================
// Zero-Copy 설계 원칙
// =============================================================================
// 1. 값 복사 금지: 포인터만 전달
// 2. Memory Pool과 함께 사용: 객체는 풀에서 할당/해제
// 3. 소유권 명확화: push 시 소유권 이전, pop 시 소유권 획득
// =============================================================================

// SPSC Lock-Free Queue (Single Producer Single Consumer)
template <typename T>
class SPSCQueue {
public:
    explicit SPSCQueue(size_t capacity)
        : capacity_(capacity)
        , mask_(capacity - 1)
        , buffer_(static_cast<T*>(std::aligned_alloc(alignof(T), sizeof(T) * capacity)))
    {
        // capacity는 2의 거듭제곱
        if ((capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be power of 2");
        }
        
        // 생성된 객체들을 위한 공간 초기화
        for (size_t i = 0; i < capacity; ++i) {
            new (&buffer_[i]) T();
        }
    }
    
    ~SPSCQueue() {
        // 남은 요소들 정리
        while (!empty()) { 
            T item; 
            pop(item); 
        }
        
        // 모든 객체 소멸자 호출
        for (size_t i = 0; i < capacity_; ++i) {
            buffer_[i].~T();
        }
        
        std::free(buffer_);
    }
    
    // Producer: 요소 추가
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & mask_;
        
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;  // Full
        }
        
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }
    
    // Producer: 요소 추가 (move)
    bool push(T&& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & mask_;
        
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;  // Full
        }
        
        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }
    
    // Consumer: 요소 추출
    bool pop(T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;  // Empty
        }
        
        item = std::move(buffer_[tail]);
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }
    
    bool empty() const {
        return head_.load(std::memory_order_acquire) == 
               tail_.load(std::memory_order_acquire);
    }
    
    // 대략적인 크기 (정확하지 않을 수 있음)
    size_t size_approx() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & mask_;
    }
    
private:
    const size_t capacity_;
    const size_t mask_;
    T* const buffer_;
    
    // False sharing 방지를 위한 cache line padding
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
};

/**
 * MPSC (Multiple Producer Single Consumer) Lock-Free Queue
 *
 * 여러 Producer가 동시에 push 가능
 * 단일 Consumer만 pop 가능
 */
template <typename T>
class MPSCQueue {
public:
    explicit MPSCQueue(size_t capacity)
        : capacity_(capacity)
        , mask_(capacity - 1)
    {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be a power of 2");
        }

        // 슬롯 배열 할당
        slots_ = static_cast<Slot*>(
            std::aligned_alloc(CACHE_LINE_SIZE, sizeof(Slot) * capacity));
        if (!slots_) {
            throw std::bad_alloc();
        }

        // 슬롯 초기화
        for (size_t i = 0; i < capacity; ++i) {
            slots_[i].turn.store(i, std::memory_order_relaxed);
        }
    }

    ~MPSCQueue() {
        // 남은 요소 정리
        T item;
        while (pop(item)) {}
        std::free(slots_);
    }

    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;

    /**
     * 요소 추가 (여러 Producer 동시 호출 가능)
     */
    bool push(const T& item) noexcept {
        size_t head = head_.load(std::memory_order_relaxed);

        while (true) {
            Slot& slot = slots_[head & mask_];
            size_t turn = slot.turn.load(std::memory_order_acquire);

            if (turn == head) {
                // 슬롯이 비어있음, CAS로 head 증가 시도
                if (head_.compare_exchange_weak(head, head + 1,
                                                std::memory_order_relaxed)) {
                    new (&slot.data) T(item);
                    slot.turn.store(head + 1, std::memory_order_release);
                    return true;
                }
            } else if (turn < head) {
                // 큐가 가득 참
                return false;
            } else {
                // 다른 Producer가 작업 중, 재시도
                head = head_.load(std::memory_order_relaxed);
            }
        }
    }

    /**
     * 요소 추출 (단일 Consumer 전용)
     */
    bool pop(T& item) noexcept {
        Slot& slot = slots_[tail_ & mask_];
        size_t turn = slot.turn.load(std::memory_order_acquire);

        if (turn != tail_ + 1) {
            return false;  // 큐가 비어있음
        }

        item = std::move(slot.data);
        slot.data.~T();
        slot.turn.store(tail_ + capacity_, std::memory_order_release);
        ++tail_;
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        size_t head = head_.load(std::memory_order_relaxed);
        return tail_ == head;
    }

    [[nodiscard]] size_t size_approx() const noexcept {
        size_t head = head_.load(std::memory_order_relaxed);
        return (head - tail_) & mask_;
    }

private:
    struct Slot {
        std::atomic<size_t> turn;
        T data;
    };

    const size_t capacity_;
    const size_t mask_;
    Slot* slots_;

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(CACHE_LINE_SIZE) size_t tail_{0};  // Consumer만 사용하므로 atomic 불필요
};

// =============================================================================
// Zero-Copy SPSC Queue (포인터 기반)
// =============================================================================
// 값 복사 없이 포인터만 전달하여 최소 지연 달성
// Memory Pool과 함께 사용하여 런타임 할당 제거
//
// 사용 예:
//   ZeroCopyQueue<Ticker> queue(1024);
//   Ticker* t = ticker_pool().create();
//   t->price = 100.0;
//   queue.push(t);  // 소유권 이전
//
//   Ticker* received = queue.pop();
//   if (received) {
//       process(*received);
//       ticker_pool().destroy(received);  // 풀에 반환
//   }
// =============================================================================
template <typename T>
class ZeroCopyQueue {
public:
    explicit ZeroCopyQueue(size_t capacity)
        : capacity_(capacity)
        , mask_(capacity - 1)
    {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be a power of 2");
        }

        // 포인터 배열 할당 (cache-line aligned)
        buffer_ = static_cast<std::atomic<T*>*>(
            std::aligned_alloc(CACHE_LINE_SIZE,
                               sizeof(std::atomic<T*>) * capacity));
        if (!buffer_) {
            throw std::bad_alloc();
        }

        // nullptr로 초기화
        for (size_t i = 0; i < capacity; ++i) {
            new (&buffer_[i]) std::atomic<T*>(nullptr);
        }
    }

    ~ZeroCopyQueue() {
        // 남은 포인터들 정리 (메모리 누수 방지)
        // 주의: 실제로는 풀에 반환해야 함
        for (size_t i = 0; i < capacity_; ++i) {
            buffer_[i].~atomic();
        }
        std::free(buffer_);
    }

    ZeroCopyQueue(const ZeroCopyQueue&) = delete;
    ZeroCopyQueue& operator=(const ZeroCopyQueue&) = delete;

    /**
     * 포인터 푸시 (소유권 이전)
     * @param ptr 전달할 포인터 (non-null)
     * @return 성공 시 true, 큐가 가득 찬 경우 false
     */
    bool push(T* ptr) noexcept {
        if (!ptr) return false;

        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & mask_;

        // 큐가 가득 찼는지 확인
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        // 포인터 저장
        buffer_[head].store(ptr, std::memory_order_relaxed);

        // head 업데이트
        head_.store(next, std::memory_order_release);
        return true;
    }

    /**
     * 포인터 팝 (소유권 획득)
     * @return 포인터 또는 nullptr (큐가 비어있는 경우)
     */
    T* pop() noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);

        // 큐가 비어있는지 확인
        if (tail == head_.load(std::memory_order_acquire)) {
            return nullptr;
        }

        // 포인터 획득
        T* ptr = buffer_[tail].load(std::memory_order_relaxed);
        buffer_[tail].store(nullptr, std::memory_order_relaxed);

        // tail 업데이트
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return ptr;
    }

    /**
     * 큐가 비어있는지 확인
     */
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    /**
     * 현재 큐에 있는 요소 수 (대략적)
     */
    [[nodiscard]] size_t size_approx() const noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_relaxed);
        return (head - tail) & mask_;
    }

    /**
     * 큐 용량
     */
    [[nodiscard]] size_t capacity() const noexcept {
        return capacity_;
    }

private:
    const size_t capacity_;
    const size_t mask_;
    std::atomic<T*>* buffer_;

    // Cache line padding으로 false sharing 방지
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
};


// =============================================================================
// Zero-Copy MPSC Queue (Multi-Producer)
// =============================================================================
template <typename T>
class ZeroCopyMPSCQueue {
public:
    explicit ZeroCopyMPSCQueue(size_t capacity)
        : capacity_(capacity)
        , mask_(capacity - 1)
    {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Capacity must be a power of 2");
        }

        slots_ = static_cast<Slot*>(
            std::aligned_alloc(CACHE_LINE_SIZE, sizeof(Slot) * capacity));
        if (!slots_) {
            throw std::bad_alloc();
        }

        for (size_t i = 0; i < capacity; ++i) {
            new (&slots_[i].turn) std::atomic<size_t>(i);
            slots_[i].ptr = nullptr;
        }
    }

    ~ZeroCopyMPSCQueue() {
        for (size_t i = 0; i < capacity_; ++i) {
            slots_[i].turn.~atomic();
        }
        std::free(slots_);
    }

    ZeroCopyMPSCQueue(const ZeroCopyMPSCQueue&) = delete;
    ZeroCopyMPSCQueue& operator=(const ZeroCopyMPSCQueue&) = delete;

    bool push(T* ptr) noexcept {
        if (!ptr) return false;

        size_t head = head_.load(std::memory_order_relaxed);

        while (true) {
            Slot& slot = slots_[head & mask_];
            size_t turn = slot.turn.load(std::memory_order_acquire);

            if (turn == head) {
                if (head_.compare_exchange_weak(head, head + 1,
                                                std::memory_order_relaxed)) {
                    slot.ptr = ptr;
                    slot.turn.store(head + 1, std::memory_order_release);
                    return true;
                }
            } else if (turn < head) {
                return false;  // 큐가 가득 참
            } else {
                head = head_.load(std::memory_order_relaxed);
            }
        }
    }

    T* pop() noexcept {
        Slot& slot = slots_[tail_ & mask_];
        size_t turn = slot.turn.load(std::memory_order_acquire);

        if (turn != tail_ + 1) {
            return nullptr;
        }

        T* ptr = slot.ptr;
        slot.ptr = nullptr;
        slot.turn.store(tail_ + capacity_, std::memory_order_release);
        ++tail_;
        return ptr;
    }

    [[nodiscard]] bool empty() const noexcept {
        return tail_ == head_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t size_approx() const noexcept {
        return (head_.load(std::memory_order_relaxed) - tail_) & mask_;
    }

private:
    struct Slot {
        std::atomic<size_t> turn;
        T* ptr;
    };

    const size_t capacity_;
    const size_t mask_;
    Slot* slots_;

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(CACHE_LINE_SIZE) size_t tail_{0};
};


// 편의를 위한 별칭
template <typename T>
using LockFreeQueue = SPSCQueue<T>;

// Zero-Copy 큐 별칭 (권장)
template <typename T>
using FastQueue = ZeroCopyQueue<T>;

template <typename T>
using FastMPSCQueue = ZeroCopyMPSCQueue<T>;

}  // namespace arbitrage