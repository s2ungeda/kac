#pragma once

/**
 * SIMD-accelerated JSON Parser (simdjson)
 *
 * AVX2/SSE4.2 기반 고속 JSON 파싱
 * - DOM API: 토큰화/검증에 SIMD 사용, 랜덤 액세스 지원
 * - 스레드 로컬 파서: 메모리 재사용으로 할당 최소화
 * - nlohmann/json 대비 ~4-6x 파싱 속도 향상
 */

#include <simdjson.h>
#include <string>
#include <string_view>
#include <cstring>
#include <cstdlib>

namespace arbitrage {

// =============================================================================
// Thread-local simdjson DOM parser (재사용으로 할당 최소화)
// =============================================================================
inline simdjson::dom::parser& thread_local_simd_parser() {
    thread_local simdjson::dom::parser parser;
    return parser;
}

// =============================================================================
// 헬퍼 함수: simdjson DOM element에서 안전한 값 추출
// =============================================================================

/// double 추출 (숫자 또는 문자열 → double)
/// Binance 등은 가격을 문자열 "2.1234"로 전송하므로 양쪽 모두 처리
inline double simd_get_double(simdjson::dom::element elem) {
    double val;
    if (elem.get(val) == simdjson::SUCCESS) return val;

    // 문자열 → double 변환 (Binance 가격 형식 "2.1234")
    std::string_view sv;
    if (elem.get(sv) == simdjson::SUCCESS && sv.size() < 64) {
        char buf[64];
        std::memcpy(buf, sv.data(), sv.size());
        buf[sv.size()] = '\0';
        return strtod(buf, nullptr);
    }

    // int64 → double (정수로 오는 가격)
    int64_t ival;
    if (elem.get(ival) == simdjson::SUCCESS) return static_cast<double>(ival);

    return 0.0;
}

/// 필드에서 double 추출 (필드 없으면 기본값)
inline double simd_get_double_or(simdjson::simdjson_result<simdjson::dom::element> result,
                                  double default_val = 0.0) {
    simdjson::dom::element elem;
    if (result.get(elem) != simdjson::SUCCESS) return default_val;
    return simd_get_double(elem);
}

/// 필드에서 string_view 추출 (필드 없으면 기본값)
/// 주의: 반환된 string_view는 parser의 내부 버퍼를 참조 — 다음 parse() 전까지만 유효
inline std::string_view simd_get_sv(simdjson::simdjson_result<simdjson::dom::element> result,
                                     std::string_view default_val = "") {
    std::string_view sv;
    if (result.get(sv) != simdjson::SUCCESS) return default_val;
    return sv;
}

/// 필드에서 int64_t 추출 (필드 없으면 기본값)
inline int64_t simd_get_int64(simdjson::simdjson_result<simdjson::dom::element> result,
                               int64_t default_val = 0) {
    int64_t val;
    if (result.get(val) != simdjson::SUCCESS) return default_val;
    return val;
}

/// 필드 존재 여부 확인
inline bool simd_has_field(simdjson::dom::element obj, std::string_view key) {
    return obj[key].error() == simdjson::SUCCESS;
}

}  // namespace arbitrage
