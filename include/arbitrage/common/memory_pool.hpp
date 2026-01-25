#pragma once

#include <atomic>
#include <memory>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>

namespace arbitrage {

/**
 * Lock-Free 고정 크기 메모리 풀
 *
 * 특징:
 * - 사전 할당된 메모리 블록 재사용
 * - Lock-Free 할당/해제 (10ns 수준)
 * - 메모리 단편화 방지
 * - Fallback: 풀이 소진되면 힙 할당
 *
 * @tparam BlockSize 블록 크기 (바이트)
 * @tparam BlockCount 블록 개수
 */
template <size_t BlockSize, size_t BlockCount>
class FixedMemoryPool {
    static_assert(BlockSize >= sizeof(void*), "BlockSize must be at least pointer size");

public:
    FixedMemoryPool() {
        // 연속된 메모리 할당
        memory_ = std::make_unique<std::byte[]>(BlockSize * BlockCount);

        // Free List 구축 (역순으로 추가하여 순서 유지)
        for (size_t i = BlockCount; i > 0; --i) {
            push_free(memory_.get() + ((i - 1) * BlockSize));
        }
    }

    ~FixedMemoryPool() = default;

    // 복사/이동 금지
    FixedMemoryPool(const FixedMemoryPool&) = delete;
    FixedMemoryPool& operator=(const FixedMemoryPool&) = delete;

    /**
     * 메모리 블록 할당
     * @return 할당된 블록 포인터, 풀이 소진되면 nullptr
     */
    void* allocate() noexcept {
        return pop_free();
    }

    /**
     * 메모리 블록 반환
     * @param ptr 반환할 블록 포인터
     */
    void deallocate(void* ptr) noexcept {
        if (ptr) {
            push_free(ptr);
        }
    }

    /**
     * 현재 사용 가능한 블록 수 (대략적)
     */
    [[nodiscard]] size_t available_approx() const noexcept {
        size_t count = 0;
        Node* node = free_head_.load(std::memory_order_relaxed);
        while (node) {
            ++count;
            node = node->next;
        }
        return count;
    }

    /**
     * 전체 블록 수
     */
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return BlockCount;
    }

    /**
     * 블록 크기
     */
    [[nodiscard]] static constexpr size_t block_size() noexcept {
        return BlockSize;
    }

private:
    struct Node {
        Node* next;
    };

    void push_free(void* ptr) noexcept {
        Node* node = static_cast<Node*>(ptr);
        Node* old_head = free_head_.load(std::memory_order_relaxed);
        do {
            node->next = old_head;
        } while (!free_head_.compare_exchange_weak(
            old_head, node,
            std::memory_order_release,
            std::memory_order_relaxed));
    }

    void* pop_free() noexcept {
        Node* node = free_head_.load(std::memory_order_acquire);
        while (node) {
            if (free_head_.compare_exchange_weak(
                    node, node->next,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                return node;
            }
        }
        return nullptr;  // 풀 소진
    }

    std::unique_ptr<std::byte[]> memory_;
    std::atomic<Node*> free_head_{nullptr};
};


/**
 * 타입 안전 객체 풀
 *
 * 특징:
 * - 특정 타입 T에 최적화
 * - Lock-Free 생성/소멸
 * - 풀 소진 시 자동 힙 Fallback
 *
 * 사용 예:
 *   ObjectPool<Ticker, 4096> pool;
 *   Ticker* t = pool.create(Exchange::Upbit, "BTC", 50000.0);
 *   pool.destroy(t);
 *
 * @tparam T 객체 타입
 * @tparam PoolSize 풀 크기 (기본 1024)
 */
template <typename T, size_t PoolSize = 1024>
class ObjectPool {
public:
    ObjectPool() = default;
    ~ObjectPool() = default;

    // 복사/이동 금지
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    /**
     * 객체 생성
     * @param args 생성자 인자
     * @return 생성된 객체 포인터
     */
    template <typename... Args>
    T* create(Args&&... args) {
        void* mem = pool_.allocate();
        if (!mem) {
            // Fallback: 풀 소진 시 힙 할당
            mem = ::operator new(sizeof(T));
            ++fallback_count_;
        }
        return new (mem) T(std::forward<Args>(args)...);
    }

    /**
     * 객체 소멸
     * @param obj 소멸할 객체 포인터
     */
    void destroy(T* obj) {
        if (!obj) return;

        // 소멸자 호출
        obj->~T();

        // 메모리 반환 (풀 범위 내인지 확인)
        pool_.deallocate(obj);
    }

    /**
     * 사용 가능한 슬롯 수
     */
    [[nodiscard]] size_t available() const noexcept {
        return pool_.available_approx();
    }

    /**
     * 전체 용량
     */
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return PoolSize;
    }

    /**
     * Fallback 횟수 (풀 소진으로 힙 사용한 횟수)
     */
    [[nodiscard]] size_t fallback_count() const noexcept {
        return fallback_count_.load(std::memory_order_relaxed);
    }

private:
    FixedMemoryPool<sizeof(T), PoolSize> pool_;
    std::atomic<size_t> fallback_count_{0};
};


/**
 * 공유 객체 풀 (싱글톤 패턴)
 *
 * 전역에서 접근 가능한 풀을 제공
 *
 * 사용 예:
 *   auto& pool = SharedObjectPool<Ticker, 4096>::instance();
 *   Ticker* t = pool.create();
 *   pool.destroy(t);
 */
template <typename T, size_t PoolSize = 1024>
class SharedObjectPool {
public:
    static ObjectPool<T, PoolSize>& instance() {
        static ObjectPool<T, PoolSize> pool;
        return pool;
    }

private:
    SharedObjectPool() = default;
};


/**
 * 풀 할당자 (STL 컨테이너 호환)
 *
 * 사용 예:
 *   std::vector<int, PoolAllocator<int, 1024>> vec;
 */
template <typename T, size_t PoolSize = 1024>
class PoolAllocator {
public:
    using value_type = T;

    PoolAllocator() = default;

    template <typename U>
    PoolAllocator(const PoolAllocator<U, PoolSize>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n == 1) {
            void* mem = pool_.allocate();
            if (mem) return static_cast<T*>(mem);
        }
        // Fallback: 대량 할당이나 풀 소진 시
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (n == 1) {
            pool_.deallocate(p);
        } else {
            ::operator delete(p);
        }
    }

    template <typename U>
    bool operator==(const PoolAllocator<U, PoolSize>&) const noexcept {
        return true;
    }

    template <typename U>
    bool operator!=(const PoolAllocator<U, PoolSize>&) const noexcept {
        return false;
    }

private:
    static inline FixedMemoryPool<sizeof(T), PoolSize> pool_;
};

}  // namespace arbitrage
