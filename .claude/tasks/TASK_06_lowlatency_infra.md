# TASK 08: 저지연 인프라 (Lock-Free Queue + Memory Pool + Spin Wait)

## 🎯 목표
스레드 간 저지연 통신 및 메모리 최적화 인프라 구축

---

## ⚠️ 왜 필요한가?

```
┌─────────────────────────────────────────────────────────────────┐
│  std::mutex + new/delete + sleep 의 문제점                      │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  std::mutex 락 획득       : 50~500ns + 컨텍스트 스위칭          │
│  new/malloc 호출          : 100~1000ns + 메모리 단편화          │
│  sleep(1ms) 실제 대기     : 2~15ms (예측 불가)                  │
│                                                                 │
│  → 김프 아비트라지: 1ms 차이로 기회 소멸!                       │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

해결책:
1. Lock-Free Queue: 락 없는 스레드 간 통신 (5~20ns)
2. Memory Pool: 사전 할당된 객체 재사용 (10ns)
3. Spin Wait: OS 스케줄러 우회, 즉각 반응 (나노초)
```

---

## 📁 생성할 파일

```
include/arbitrage/common/
├── lockfree_queue.hpp      # SPSC/MPMC Queue
├── memory_pool.hpp         # Object Pool
├── spin_wait.hpp           # Spin Wait 유틸리티
└── pooled_types.hpp        # 풀링 대상 타입

third_party/
└── rigtorp/
    └── SPSCQueue.h         # rigtorp 라이브러리 (헤더 온리)

src/common/
└── memory_pool.cpp

tests/unit/common/
├── lockfree_queue_test.cpp
├── memory_pool_test.cpp
└── spin_wait_benchmark.cpp
```

---

## 📝 Part 1: Lock-Free Queue

### SPSC Queue (Single Producer Single Consumer)

```cpp
#pragma once

#include <atomic>
#include <cstddef>

namespace arbitrage {

// Cache line padding
constexpr std::size_t CACHE_LINE_SIZE = 64;

// SPSC Lock-Free Queue (WebSocket → Strategy)
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
    }
    
    ~SPSCQueue() {
        while (!empty()) { T item; pop(item); }
        std::free(buffer_);
    }
    
    // Producer: 요소 추가
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & mask_;
        
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;  // Full
        }
        
        new (&buffer_[head]) T(item);
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
        buffer_[tail].~T();
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }
    
    bool empty() const {
        return head_.load(std::memory_order_acquire) == 
               tail_.load(std::memory_order_acquire);
    }
    
private:
    const size_t capacity_;
    const size_t mask_;
    T* const buffer_;
    
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
};

template <typename T>
using LockFreeQueue = SPSCQueue<T>;

}  // namespace arbitrage
```

---

## 📝 Part 2: Memory Pool

### Object Pool (타입 안전)

```cpp
#pragma once

#include <atomic>
#include <memory>

namespace arbitrage {

// Lock-Free 고정 크기 메모리 풀
template <size_t BlockSize, size_t BlockCount>
class FixedMemoryPool {
public:
    FixedMemoryPool() {
        memory_ = std::make_unique<std::byte[]>(BlockSize * BlockCount);
        
        // Free List 구축
        for (size_t i = 0; i < BlockCount; ++i) {
            push_free(memory_.get() + (i * BlockSize));
        }
    }
    
    void* allocate() noexcept { return pop_free(); }
    void deallocate(void* ptr) noexcept { if (ptr) push_free(ptr); }
    
private:
    struct Node { Node* next; };
    
    void push_free(void* ptr) noexcept {
        Node* node = static_cast<Node*>(ptr);
        Node* old_head = free_head_.load(std::memory_order_relaxed);
        do {
            node->next = old_head;
        } while (!free_head_.compare_exchange_weak(old_head, node));
    }
    
    void* pop_free() noexcept {
        Node* node = free_head_.load(std::memory_order_acquire);
        while (node && !free_head_.compare_exchange_weak(node, node->next));
        return node;
    }
    
    std::unique_ptr<std::byte[]> memory_;
    std::atomic<Node*> free_head_{nullptr};
};

// 타입 안전 객체 풀
template <typename T, size_t PoolSize = 1024>
class ObjectPool {
public:
    template <typename... Args>
    T* create(Args&&... args) {
        void* mem = pool_.allocate();
        if (!mem) mem = ::operator new(sizeof(T));  // Fallback
        return new (mem) T(std::forward<Args>(args)...);
    }
    
    void destroy(T* obj) {
        if (!obj) return;
        obj->~T();
        pool_.deallocate(obj);
    }
    
private:
    FixedMemoryPool<sizeof(T), PoolSize> pool_;
};

}  // namespace arbitrage
```

### 풀링 대상 타입

```cpp
// pooled_types.hpp
namespace arbitrage {

// 시세 데이터 풀
inline ObjectPool<Ticker, 4096>& ticker_pool() {
    static ObjectPool<Ticker, 4096> pool;
    return pool;
}

// 호가 데이터 풀
inline ObjectPool<OrderBook, 1024>& orderbook_pool() {
    static ObjectPool<OrderBook, 1024> pool;
    return pool;
}

// 주문 객체 풀
inline ObjectPool<Order, 256>& order_pool() {
    static ObjectPool<Order, 256> pool;
    return pool;
}

}
```

---

## 📝 Part 3: Spin Wait

### Spin Wait 유틸리티

```cpp
#pragma once

#include <atomic>
#include <emmintrin.h>  // _mm_pause

namespace arbitrage {

class SpinWait {
public:
    // 단순 스핀
    static void spin() noexcept {
        _mm_pause();  // CPU 친화적 대기
    }
    
    // 조건 대기
    template <typename Predicate>
    static void until(Predicate&& pred) noexcept {
        while (!pred()) {
            _mm_pause();
        }
    }
    
    // 적응형 스핀 (스핀 → yield → sleep)
    class Adaptive {
    public:
        void wait() noexcept {
            if (count_++ < 10) {
                _mm_pause();
            } else if (count_ < 20) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        }
        void reset() noexcept { count_ = 0; }
    private:
        size_t count_{0};
    };
};

// Spin Lock (mutex 대체)
class SpinLock {
public:
    void lock() noexcept {
        while (locked_.exchange(true, std::memory_order_acquire)) {
            while (locked_.load(std::memory_order_relaxed)) {
                _mm_pause();
            }
        }
    }
    
    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
    }
    
private:
    std::atomic<bool> locked_{false};
};

}  // namespace arbitrage
```

---

## 🔧 통합 사용 예시

```cpp
#include "arbitrage/common/lockfree_queue.hpp"
#include "arbitrage/common/memory_pool.hpp"
#include "arbitrage/common/spin_wait.hpp"

// WebSocket 스레드 (Producer)
void websocket_thread() {
    while (running) {
        // 풀에서 Ticker 객체 할당
        auto* ticker = ticker_pool().create();
        parse_message(*ticker);
        
        // Lock-Free Queue에 푸시
        while (!ticker_queue.push(ticker)) {
            SpinWait::spin();  // Full이면 스핀
        }
    }
}

// Strategy 스레드 (Consumer)
void strategy_thread() {
    while (running) {
        Ticker* ticker = nullptr;
        
        // Spin Polling으로 이벤트 대기
        if (ticker_queue.pop(ticker)) {
            process_ticker(*ticker);
            ticker_pool().destroy(ticker);  // 풀로 반환
        } else {
            SpinWait::spin();  // Empty면 스핀
        }
    }
}
```

---

## 📊 성능 비교

| 항목 | 기존 방식 | 최적화 후 | 개선 |
|------|----------|----------|:----:|
| 스레드 통신 | mutex: 500ns | Lock-Free: 20ns | **25x** |
| 객체 할당 | new: 500ns | Pool: 10ns | **50x** |
| 이벤트 대기 | sleep: 5ms | Spin: 100ns | **50000x** |

---

## ✅ 완료 조건 체크리스트

```
□ SPSC Queue 구현
□ MPMC Queue 구현 (선택)
□ FixedMemoryPool 구현
□ ObjectPool<T> 구현
□ SpinWait 유틸리티
□ SpinLock 구현
□ Ticker/OrderBook/Order 풀 정의
□ Cache line padding (False Sharing 방지)
□ 단위 테스트
□ 벤치마크 테스트
```

---

## 🔗 의존 관계

```
TASK_01 (프로젝트 셋업) 완료 필요
```

---

## 📎 다음 태스크

완료 후: TASK_09_rate_limiter_parser.md
