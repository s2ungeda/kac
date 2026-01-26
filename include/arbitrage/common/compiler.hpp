#pragma once

/**
 * 컴파일러 최적화 매크로
 *
 * 분기 예측, 인라이닝 힌트 등 저지연 최적화를 위한 매크로
 * 실제 사용되는 매크로만 정의 (YAGNI 원칙)
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

}  // namespace arbitrage
