#pragma once

/**
 * NUMA Allocator (TASK_20)
 *
 * NUMA 인식 메모리 할당자
 * - NumaAllocator: STL 호환 할당자
 * - NumaBuffer: NUMA 지역 버퍼
 */

#include <memory>
#include <cstddef>
#include <new>

namespace arbitrage {

// =============================================================================
// NUMA 유틸리티 함수
// =============================================================================

/**
 * 현재 스레드의 NUMA 노드
 */
int get_current_numa_node();

/**
 * 메모리 주소가 속한 NUMA 노드
 */
int get_memory_numa_node(void* ptr);

/**
 * NUMA 지원 여부
 */
bool is_numa_available();

/**
 * NUMA 노드에 메모리 할당
 */
void* numa_alloc(size_t size, int numa_node = -1);

/**
 * NUMA 메모리 해제
 */
void numa_free(void* ptr, size_t size);

// =============================================================================
// NUMA 인식 할당자 (STL 호환)
// =============================================================================
template<typename T>
class NumaAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;

    /**
     * 생성자
     * @param numa_node NUMA 노드 (-1 = 현재 스레드의 노드)
     */
    explicit NumaAllocator(int numa_node = -1) noexcept
        : numa_node_(numa_node)
    {}

    template<typename U>
    NumaAllocator(const NumaAllocator<U>& other) noexcept
        : numa_node_(other.numa_node())
    {}

    /**
     * 메모리 할당
     */
    T* allocate(std::size_t n) {
        if (n > std::size_t(-1) / sizeof(T)) {
            throw std::bad_array_new_length();
        }

        size_t bytes = n * sizeof(T);

        if (is_numa_available() && numa_node_ >= 0) {
            void* ptr = numa_alloc(bytes, numa_node_);
            if (ptr) {
                return static_cast<T*>(ptr);
            }
        }

        // NUMA 미지원 또는 실패 시 일반 할당
        void* ptr = ::operator new(bytes);
        return static_cast<T*>(ptr);
    }

    /**
     * 메모리 해제
     */
    void deallocate(T* p, std::size_t n) noexcept {
        if (is_numa_available() && numa_node_ >= 0) {
            numa_free(p, n * sizeof(T));
        } else {
            ::operator delete(p);
        }
    }

    /**
     * NUMA 노드 조회
     */
    int numa_node() const noexcept { return numa_node_; }

    /**
     * 비교 연산자
     */
    template<typename U>
    bool operator==(const NumaAllocator<U>& other) const noexcept {
        return numa_node_ == other.numa_node();
    }

    template<typename U>
    bool operator!=(const NumaAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

private:
    int numa_node_;
};

// =============================================================================
// NUMA 인식 버퍼
// =============================================================================
class NumaBuffer {
public:
    /**
     * 생성자
     * @param size 버퍼 크기
     * @param numa_node NUMA 노드 (-1 = 현재 스레드의 노드)
     */
    NumaBuffer(size_t size, int numa_node = -1);

    ~NumaBuffer();

    NumaBuffer(const NumaBuffer&) = delete;
    NumaBuffer& operator=(const NumaBuffer&) = delete;

    NumaBuffer(NumaBuffer&& other) noexcept;
    NumaBuffer& operator=(NumaBuffer&& other) noexcept;

    /**
     * 데이터 포인터
     */
    void* data() { return data_; }
    const void* data() const { return data_; }

    /**
     * 타입 캐스팅 접근
     */
    template<typename T>
    T* as() { return static_cast<T*>(data_); }

    template<typename T>
    const T* as() const { return static_cast<const T*>(data_); }

    /**
     * 버퍼 크기
     */
    size_t size() const { return size_; }

    /**
     * NUMA 노드
     */
    int numa_node() const { return numa_node_; }

    /**
     * 유효성 검사
     */
    bool valid() const { return data_ != nullptr; }

private:
    void* data_ = nullptr;
    size_t size_ = 0;
    int numa_node_ = -1;
    bool numa_allocated_ = false;
};

// =============================================================================
// NUMA 인식 unique_ptr
// =============================================================================
template<typename T>
struct NumaDeleter {
    int numa_node;
    size_t count;

    void operator()(T* ptr) const noexcept {
        if (ptr) {
            // 객체 소멸
            if constexpr (!std::is_trivially_destructible_v<T>) {
                for (size_t i = 0; i < count; ++i) {
                    ptr[i].~T();
                }
            }

            // 메모리 해제
            if (is_numa_available() && numa_node >= 0) {
                numa_free(ptr, count * sizeof(T));
            } else {
                ::operator delete(ptr);
            }
        }
    }
};

template<typename T>
using NumaUniquePtr = std::unique_ptr<T, NumaDeleter<T>>;

/**
 * NUMA 노드에 객체 생성
 */
template<typename T, typename... Args>
NumaUniquePtr<T> make_numa_unique(int numa_node, Args&&... args) {
    void* ptr = nullptr;

    if (is_numa_available() && numa_node >= 0) {
        ptr = numa_alloc(sizeof(T), numa_node);
    }

    if (!ptr) {
        ptr = ::operator new(sizeof(T));
    }

    try {
        T* obj = new (ptr) T(std::forward<Args>(args)...);
        return NumaUniquePtr<T>(obj, NumaDeleter<T>{numa_node, 1});
    } catch (...) {
        if (is_numa_available() && numa_node >= 0) {
            numa_free(ptr, sizeof(T));
        } else {
            ::operator delete(ptr);
        }
        throw;
    }
}

}  // namespace arbitrage
