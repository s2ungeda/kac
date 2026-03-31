#pragma once

/**
 * Symbol Master (TASK_18)
 *
 * 거래소별 심볼 매핑 관리
 * - 통합 심볼 ↔ 거래소 네이티브 심볼 변환
 * - 거래 제한 정보 (최소/최대 수량, 단위)
 * - 수량 정규화
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/error.hpp"

#include <string>
#include <map>
#include <optional>
#include "arbitrage/common/spin_wait.hpp"
#include <vector>
#include <chrono>

namespace arbitrage {

// =============================================================================
// 심볼 정보
// =============================================================================
struct SymbolInfo {
    std::string base;           // 기준 통화 (예: "XRP")
    std::string quote;          // 호가 통화 (예: "KRW", "USDT")
    std::string native;         // 거래소 네이티브 심볼 (예: "KRW-XRP")
    std::string unified;        // 통합 심볼 (예: "XRP/KRW")

    Exchange exchange;          // 거래소

    // 거래 제한
    double min_qty{0.0};        // 최소 주문 수량
    double max_qty{0.0};        // 최대 주문 수량 (0 = 무제한)
    double qty_step{0.0};       // 수량 단위 (예: 0.01)
    int qty_precision{0};       // 수량 소수점 자릿수

    double min_price{0.0};      // 최소 가격
    double max_price{0.0};      // 최대 가격 (0 = 무제한)
    double price_step{0.0};     // 가격 단위
    int price_precision{0};     // 가격 소수점 자릿수

    double min_notional{0.0};   // 최소 주문 금액

    // 상태
    bool trading_enabled{true}; // 거래 가능 여부
    bool deposit_enabled{true}; // 입금 가능 여부
    bool withdraw_enabled{true};// 출금 가능 여부

    // 메타데이터
    std::chrono::system_clock::time_point updated_at;

    // 수량 정규화
    double normalize_qty(double qty) const;

    // 가격 정규화
    double normalize_price(double price) const;

    // 유효성 검사
    bool is_valid_qty(double qty) const;
    bool is_valid_price(double price) const;
    bool is_valid_notional(double qty, double price) const;
};

// =============================================================================
// 거래소별 심볼 포맷
// =============================================================================
namespace symbol_format {

// 통합 심볼 형식: "BASE/QUOTE" (예: "XRP/KRW", "XRP/USDT")
// Upbit:   "QUOTE-BASE" (예: "KRW-XRP")
// Bithumb: "BASE_QUOTE" (예: "XRP_KRW")
// Binance: "BASEQUOTE"  (예: "XRPUSDT")
// MEXC:    "BASEQUOTE"  (예: "XRPUSDT")

std::string to_upbit(const std::string& base, const std::string& quote);
std::string to_bithumb(const std::string& base, const std::string& quote);
std::string to_binance(const std::string& base, const std::string& quote);
std::string to_mexc(const std::string& base, const std::string& quote);

std::string to_native(Exchange ex, const std::string& base, const std::string& quote);
std::string to_unified(const std::string& base, const std::string& quote);

// 파싱
bool parse_upbit(const std::string& native, std::string& base, std::string& quote);
bool parse_bithumb(const std::string& native, std::string& base, std::string& quote);
bool parse_binance(const std::string& native, std::string& base, std::string& quote);
bool parse_mexc(const std::string& native, std::string& base, std::string& quote);

bool parse_native(Exchange ex, const std::string& native, std::string& base, std::string& quote);
bool parse_unified(const std::string& unified, std::string& base, std::string& quote);

}  // namespace symbol_format

// =============================================================================
// 심볼 마스터
// =============================================================================
class SymbolMaster {
public:
    SymbolMaster();
    ~SymbolMaster() = default;

    // 복사/이동 금지
    SymbolMaster(const SymbolMaster&) = delete;
    SymbolMaster& operator=(const SymbolMaster&) = delete;

    // =========================================================================
    // 심볼 변환
    // =========================================================================

    /**
     * 통합 심볼 → 네이티브 심볼 변환
     * @param exchange 거래소
     * @param unified 통합 심볼 (예: "XRP/KRW")
     * @return 네이티브 심볼 (예: "KRW-XRP")
     */
    std::string to_native(Exchange exchange, const std::string& unified) const;

    /**
     * 네이티브 심볼 → 통합 심볼 변환
     * @param exchange 거래소
     * @param native 네이티브 심볼 (예: "KRW-XRP")
     * @return 통합 심볼 (예: "XRP/KRW")
     */
    std::string to_unified(Exchange exchange, const std::string& native) const;

    /**
     * 기준/호가 통화로 네이티브 심볼 생성
     */
    std::string make_native(Exchange exchange, const std::string& base, const std::string& quote) const;

    // =========================================================================
    // 심볼 정보 관리
    // =========================================================================

    /**
     * 심볼 정보 등록
     * @param info 심볼 정보
     */
    void register_symbol(const SymbolInfo& info);

    /**
     * 심볼 정보 조회
     * @param exchange 거래소
     * @param symbol 심볼 (통합 또는 네이티브)
     * @return 심볼 정보
     */
    std::optional<SymbolInfo> get_info(Exchange exchange, const std::string& symbol) const;

    /**
     * 심볼 정보 조회 (base/quote로)
     */
    std::optional<SymbolInfo> get_info(
        Exchange exchange,
        const std::string& base,
        const std::string& quote
    ) const;

    /**
     * 거래소의 모든 심볼 조회
     */
    std::vector<SymbolInfo> get_symbols(Exchange exchange) const;

    /**
     * 특정 기준 통화의 모든 심볼 조회 (거래소 무관)
     */
    std::vector<SymbolInfo> get_symbols_by_base(const std::string& base) const;

    /**
     * 심볼 존재 여부 확인
     */
    bool has_symbol(Exchange exchange, const std::string& symbol) const;

    // =========================================================================
    // 수량/가격 정규화
    // =========================================================================

    /**
     * 수량 정규화 (거래소 규칙에 맞게)
     * @param exchange 거래소
     * @param symbol 심볼
     * @param qty 수량
     * @return 정규화된 수량
     */
    double normalize_qty(Exchange exchange, const std::string& symbol, double qty) const;

    /**
     * 가격 정규화
     */
    double normalize_price(Exchange exchange, const std::string& symbol, double price) const;

    /**
     * 주문 유효성 검사
     * @return (유효 여부, 오류 메시지)
     */
    std::pair<bool, std::string> validate_order(
        Exchange exchange,
        const std::string& symbol,
        double qty,
        double price
    ) const;

    // =========================================================================
    // 기본값 설정
    // =========================================================================

    /**
     * XRP 기본 심볼 정보 초기화
     */
    void init_xrp_defaults();

    /**
     * 심볼 정보 업데이트 (API에서 조회)
     */
    void update_from_exchange(Exchange exchange, const std::vector<SymbolInfo>& symbols);

    // =========================================================================
    // 설정 파일
    // =========================================================================

    /**
     * 파일로 저장
     */
    Result<void> save_to_file(const std::string& path) const;

    /**
     * 파일에서 로드
     */
    Result<void> load_from_file(const std::string& path);

    // =========================================================================
    // 통계
    // =========================================================================

    size_t count() const;
    size_t count(Exchange exchange) const;

private:
    // 키 생성: "exchange:unified"
    static std::string make_key(Exchange exchange, const std::string& unified);

    mutable RWSpinLock mutex_;

    // unified 심볼 기준 저장
    std::map<std::string, SymbolInfo> symbols_;  // "exchange:unified" -> SymbolInfo

    // 역방향 매핑 (native -> unified)
    std::map<std::string, std::string> native_to_unified_;  // "exchange:native" -> unified
};

// =============================================================================
// 글로벌 인스턴스 접근자
// =============================================================================
SymbolMaster& symbol_master();

// =============================================================================
// 편의 함수
// =============================================================================

/**
 * XRP 통합 심볼 (KRW 거래소용)
 */
constexpr const char* XRP_KRW = "XRP/KRW";

/**
 * XRP 통합 심볼 (USDT 거래소용)
 */
constexpr const char* XRP_USDT = "XRP/USDT";

/**
 * 거래소에 맞는 XRP 심볼 반환
 */
inline std::string xrp_symbol(Exchange exchange) {
    return is_krw_exchange(exchange) ? XRP_KRW : XRP_USDT;
}

}  // namespace arbitrage
