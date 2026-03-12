/**
 * NUMA Allocator Implementation (TASK_20)
 *
 * NUMA 인식 메모리 할당
 * - libnuma 있으면 사용
 * - 없으면 일반 할당으로 폴백
 */

#include "arbitrage/common/numa_allocator.hpp"

#include <cstdlib>
#include <cstring>

// NUMA 라이브러리 헤더 (선택적)
#ifdef __linux__
#include <unistd.h>
#include <sys/mman.h>

// libnuma 사용 비활성화 (대부분의 시스템에서 미설치)
// libnuma가 필요하면 CMakeLists.txt에서 링크 추가 필요
#define HAS_LIBNUMA 0

#endif  // __linux__

namespace arbitrage {

// =============================================================================
// NUMA 유틸리티 함수
// =============================================================================

bool is_numa_available() {
#if HAS_LIBNUMA
    return numa_available() >= 0;
#else
    return false;
#endif
}

int get_current_numa_node() {
#if HAS_LIBNUMA
    if (numa_available() >= 0) {
        // numa_preferred()가 더 간단
        return numa_preferred();
    }
#endif
    return 0;
}

int get_memory_numa_node(void* ptr) {
#if HAS_LIBNUMA
    if (numa_available() >= 0 && ptr) {
        // libnuma로 노드 조회
        int node = numa_node_of_cpu(0);  // 단순화
        return node;
    }
#else
    (void)ptr;
#endif
    return 0;
}

void* numa_alloc(size_t size, int numa_node) {
#if HAS_LIBNUMA
    if (numa_available() >= 0) {
        if (numa_node >= 0 && numa_node <= numa_max_node()) {
            return numa_alloc_onnode(size, numa_node);
        }
        return numa_alloc_local(size);
    }
#else
    (void)numa_node;
#endif

    // NUMA 미지원 시 정렬된 메모리 할당
    void* ptr = nullptr;
#ifdef __linux__
    if (posix_memalign(&ptr, 64, size) != 0) {
        return nullptr;
    }
#else
    ptr = std::aligned_alloc(64, (size + 63) & ~63);
#endif
    return ptr;
}

void numa_free(void* ptr, size_t size) {
    if (!ptr) return;

#if HAS_LIBNUMA
    if (numa_available() >= 0) {
        numa_free(ptr, size);
        return;
    }
#else
    (void)size;
#endif

    std::free(ptr);
}

// =============================================================================
// NumaBuffer 구현
// =============================================================================

NumaBuffer::NumaBuffer(size_t size, int numa_node)
    : size_(size)
    , numa_node_(numa_node)
{
    if (size_ == 0) {
        return;
    }

#if HAS_LIBNUMA
    if (is_numa_available() && numa_node_ >= 0) {
        data_ = numa_alloc_onnode(size_, numa_node_);
        if (data_) {
            numa_allocated_ = true;
            std::memset(data_, 0, size_);
            return;
        }
    }
#endif

    // NUMA 미지원 또는 실패 시
    void* ptr = nullptr;
#ifdef __linux__
    if (posix_memalign(&ptr, 64, size_) == 0) {
        data_ = ptr;
        std::memset(data_, 0, size_);
    }
#else
    ptr = std::aligned_alloc(64, (size_ + 63) & ~63);
    if (ptr) {
        data_ = ptr;
        std::memset(data_, 0, size_);
    }
#endif
}

NumaBuffer::~NumaBuffer() {
    if (data_) {
#if HAS_LIBNUMA
        if (numa_allocated_ && is_numa_available()) {
            numa_free(data_, size_);
            return;
        }
#endif
        std::free(data_);
    }
}

NumaBuffer::NumaBuffer(NumaBuffer&& other) noexcept
    : data_(other.data_)
    , size_(other.size_)
    , numa_node_(other.numa_node_)
    , numa_allocated_(other.numa_allocated_)
{
    other.data_ = nullptr;
    other.size_ = 0;
}

NumaBuffer& NumaBuffer::operator=(NumaBuffer&& other) noexcept {
    if (this != &other) {
        // 기존 메모리 해제
        if (data_) {
#if HAS_LIBNUMA
            if (numa_allocated_ && is_numa_available()) {
                numa_free(data_, size_);
            } else
#endif
            {
                std::free(data_);
            }
        }

        // 이동
        data_ = other.data_;
        size_ = other.size_;
        numa_node_ = other.numa_node_;
        numa_allocated_ = other.numa_allocated_;

        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

}  // namespace arbitrage
