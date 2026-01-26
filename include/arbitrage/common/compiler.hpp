#pragma once

/**
 * 컴파일러 최적화 매크로 및 힌트
 *
 * 분기 예측, 인라이닝, 캐시 힌트 등 저지연 최적화를 위한 매크로 모음
 */

namespace arbitrage {

// =============================================================================
// 분기 예측 힌트 (Branch Prediction)
// =============================================================================

#if defined(__GNUC__) || defined(__clang__)
    #define LIKELY(x)   __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)
#endif

// =============================================================================
// 인라인 힌트
// =============================================================================

#if defined(__GNUC__) || defined(__clang__)
    #define FORCE_INLINE inline __attribute__((always_inline))
    #define NO_INLINE    __attribute__((noinline))
#elif defined(_MSC_VER)
    #define FORCE_INLINE __forceinline
    #define NO_INLINE    __declspec(noinline)
#else
    #define FORCE_INLINE inline
    #define NO_INLINE
#endif

// =============================================================================
// 핫/콜드 함수 힌트
// =============================================================================

#if defined(__GNUC__) || defined(__clang__)
    // 자주 호출되는 함수 (캐시 최적화)
    #define HOT_FUNCTION  __attribute__((hot))
    // 드물게 호출되는 함수 (에러 처리 등)
    #define COLD_FUNCTION __attribute__((cold))
#else
    #define HOT_FUNCTION
    #define COLD_FUNCTION
#endif

// =============================================================================
// 메모리 프리페치 힌트
// =============================================================================

#if defined(__GNUC__) || defined(__clang__)
    // 읽기용 프리페치
    #define PREFETCH_READ(addr)  __builtin_prefetch((addr), 0, 3)
    // 쓰기용 프리페치
    #define PREFETCH_WRITE(addr) __builtin_prefetch((addr), 1, 3)
    // 낮은 지역성 (곧 사용 안 함)
    #define PREFETCH_NTA(addr)   __builtin_prefetch((addr), 0, 0)
#else
    #define PREFETCH_READ(addr)
    #define PREFETCH_WRITE(addr)
    #define PREFETCH_NTA(addr)
#endif

// =============================================================================
// Unreachable 힌트 (도달 불가능 코드)
// =============================================================================

#if defined(__GNUC__) || defined(__clang__)
    #define UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
    #define UNREACHABLE() __assume(0)
#else
    #define UNREACHABLE() ((void)0)
#endif

// =============================================================================
// Assume 힌트 (조건 가정)
// =============================================================================

#if defined(__clang__)
    #define ASSUME(cond) __builtin_assume(cond)
#elif defined(__GNUC__)
    #define ASSUME(cond) do { if (!(cond)) __builtin_unreachable(); } while(0)
#elif defined(_MSC_VER)
    #define ASSUME(cond) __assume(cond)
#else
    #define ASSUME(cond) ((void)0)
#endif

// =============================================================================
// 정렬 힌트
// =============================================================================

#define CACHE_ALIGNED alignas(64)

// =============================================================================
// Restrict 포인터 (앨리어싱 없음 보장)
// =============================================================================

#if defined(__GNUC__) || defined(__clang__)
    #define RESTRICT __restrict__
#elif defined(_MSC_VER)
    #define RESTRICT __restrict
#else
    #define RESTRICT
#endif

}  // namespace arbitrage
