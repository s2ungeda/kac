/**
 * Symbol Master Implementation (TASK_18)
 */

#include "arbitrage/common/symbol_master.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace arbitrage {

// =============================================================================
// 글로벌 인스턴스
// =============================================================================
SymbolMaster& symbol_master() {
    static SymbolMaster instance;
    return instance;
}

// =============================================================================
// SymbolInfo 메서드
// =============================================================================
double SymbolInfo::normalize_qty(double qty) const {
    if (qty_step > 0.0) {
        // 단위에 맞게 내림
        qty = std::floor(qty / qty_step) * qty_step;
    }

    if (qty_precision > 0) {
        double factor = std::pow(10.0, qty_precision);
        qty = std::floor(qty * factor) / factor;
    }

    return qty;
}

double SymbolInfo::normalize_price(double price) const {
    if (price_step > 0.0) {
        price = std::round(price / price_step) * price_step;
    }

    if (price_precision > 0) {
        double factor = std::pow(10.0, price_precision);
        price = std::round(price * factor) / factor;
    }

    return price;
}

bool SymbolInfo::is_valid_qty(double qty) const {
    if (qty < min_qty) return false;
    if (max_qty > 0.0 && qty > max_qty) return false;
    return true;
}

bool SymbolInfo::is_valid_price(double price) const {
    if (price < min_price) return false;
    if (max_price > 0.0 && price > max_price) return false;
    return true;
}

bool SymbolInfo::is_valid_notional(double qty, double price) const {
    double notional = qty * price;
    return notional >= min_notional;
}

// =============================================================================
// 심볼 포맷 함수
// =============================================================================
namespace symbol_format {

std::string to_upbit(const std::string& base, const std::string& quote) {
    // Upbit: "QUOTE-BASE" (예: "KRW-XRP")
    return quote + "-" + base;
}

std::string to_bithumb(const std::string& base, const std::string& quote) {
    // Bithumb: "BASE_QUOTE" (예: "XRP_KRW")
    return base + "_" + quote;
}

std::string to_binance(const std::string& base, const std::string& quote) {
    // Binance: "BASEQUOTE" (예: "XRPUSDT")
    return base + quote;
}

std::string to_mexc(const std::string& base, const std::string& quote) {
    // MEXC: "BASEQUOTE" (예: "XRPUSDT")
    return base + quote;
}

std::string to_native(Exchange ex, const std::string& base, const std::string& quote) {
    switch (ex) {
        case Exchange::Upbit:   return to_upbit(base, quote);
        case Exchange::Bithumb: return to_bithumb(base, quote);
        case Exchange::Binance: return to_binance(base, quote);
        case Exchange::MEXC:    return to_mexc(base, quote);
        default:                return base + "/" + quote;
    }
}

std::string to_unified(const std::string& base, const std::string& quote) {
    return base + "/" + quote;
}

bool parse_upbit(const std::string& native, std::string& base, std::string& quote) {
    // "KRW-XRP" -> base="XRP", quote="KRW"
    auto pos = native.find('-');
    if (pos == std::string::npos) return false;

    quote = native.substr(0, pos);
    base = native.substr(pos + 1);
    return !base.empty() && !quote.empty();
}

bool parse_bithumb(const std::string& native, std::string& base, std::string& quote) {
    // "XRP_KRW" -> base="XRP", quote="KRW"
    auto pos = native.find('_');
    if (pos == std::string::npos) return false;

    base = native.substr(0, pos);
    quote = native.substr(pos + 1);
    return !base.empty() && !quote.empty();
}

bool parse_binance(const std::string& native, std::string& base, std::string& quote) {
    // "XRPUSDT" -> base="XRP", quote="USDT"
    // 알려진 quote 통화 확인
    static const std::vector<std::string> quotes = {"USDT", "USDC", "BUSD", "BTC", "ETH", "BNB"};

    for (const auto& q : quotes) {
        if (native.size() > q.size() && native.substr(native.size() - q.size()) == q) {
            base = native.substr(0, native.size() - q.size());
            quote = q;
            return !base.empty();
        }
    }

    return false;
}

bool parse_mexc(const std::string& native, std::string& base, std::string& quote) {
    // MEXC는 Binance와 동일한 형식
    return parse_binance(native, base, quote);
}

bool parse_native(Exchange ex, const std::string& native, std::string& base, std::string& quote) {
    switch (ex) {
        case Exchange::Upbit:   return parse_upbit(native, base, quote);
        case Exchange::Bithumb: return parse_bithumb(native, base, quote);
        case Exchange::Binance: return parse_binance(native, base, quote);
        case Exchange::MEXC:    return parse_mexc(native, base, quote);
        default:                return false;
    }
}

bool parse_unified(const std::string& unified, std::string& base, std::string& quote) {
    // "XRP/KRW" -> base="XRP", quote="KRW"
    auto pos = unified.find('/');
    if (pos == std::string::npos) return false;

    base = unified.substr(0, pos);
    quote = unified.substr(pos + 1);
    return !base.empty() && !quote.empty();
}

}  // namespace symbol_format

// =============================================================================
// SymbolMaster 생성자
// =============================================================================
SymbolMaster::SymbolMaster() {
    init_xrp_defaults();
}

// =============================================================================
// 심볼 변환
// =============================================================================
std::string SymbolMaster::to_native(Exchange exchange, const std::string& unified) const {
    std::string base, quote;
    if (!symbol_format::parse_unified(unified, base, quote)) {
        return unified;  // 파싱 실패 시 원본 반환
    }

    return symbol_format::to_native(exchange, base, quote);
}

std::string SymbolMaster::to_unified(Exchange exchange, const std::string& native) const {
    // 캐시된 매핑 확인
    std::string key = std::to_string(static_cast<int>(exchange)) + ":" + native;
    {
        std::shared_lock lock(mutex_);
        auto it = native_to_unified_.find(key);
        if (it != native_to_unified_.end()) {
            return it->second;
        }
    }

    // 파싱
    std::string base, quote;
    if (!symbol_format::parse_native(exchange, native, base, quote)) {
        return native;  // 파싱 실패 시 원본 반환
    }

    return symbol_format::to_unified(base, quote);
}

std::string SymbolMaster::make_native(Exchange exchange, const std::string& base, const std::string& quote) const {
    return symbol_format::to_native(exchange, base, quote);
}

// =============================================================================
// 심볼 정보 관리
// =============================================================================
void SymbolMaster::register_symbol(const SymbolInfo& info) {
    std::string key = make_key(info.exchange, info.unified);

    std::unique_lock lock(mutex_);
    symbols_[key] = info;

    // 역방향 매핑 등록
    std::string native_key = std::to_string(static_cast<int>(info.exchange)) + ":" + info.native;
    native_to_unified_[native_key] = info.unified;
}

std::optional<SymbolInfo> SymbolMaster::get_info(Exchange exchange, const std::string& symbol) const {
    std::shared_lock lock(mutex_);

    // unified로 먼저 검색
    std::string key = make_key(exchange, symbol);
    auto it = symbols_.find(key);
    if (it != symbols_.end()) {
        return it->second;
    }

    // native로 검색 (unified로 변환 후)
    std::string unified = to_unified(exchange, symbol);
    if (unified != symbol) {
        key = make_key(exchange, unified);
        it = symbols_.find(key);
        if (it != symbols_.end()) {
            return it->second;
        }
    }

    return std::nullopt;
}

std::optional<SymbolInfo> SymbolMaster::get_info(
    Exchange exchange,
    const std::string& base,
    const std::string& quote
) const {
    std::string unified = symbol_format::to_unified(base, quote);
    return get_info(exchange, unified);
}

std::vector<SymbolInfo> SymbolMaster::get_symbols(Exchange exchange) const {
    std::shared_lock lock(mutex_);

    std::vector<SymbolInfo> result;
    for (const auto& [key, info] : symbols_) {
        if (info.exchange == exchange) {
            result.push_back(info);
        }
    }

    return result;
}

std::vector<SymbolInfo> SymbolMaster::get_symbols_by_base(const std::string& base) const {
    std::shared_lock lock(mutex_);

    std::vector<SymbolInfo> result;
    for (const auto& [key, info] : symbols_) {
        if (info.base == base) {
            result.push_back(info);
        }
    }

    return result;
}

bool SymbolMaster::has_symbol(Exchange exchange, const std::string& symbol) const {
    return get_info(exchange, symbol).has_value();
}

// =============================================================================
// 수량/가격 정규화
// =============================================================================
double SymbolMaster::normalize_qty(Exchange exchange, const std::string& symbol, double qty) const {
    auto info = get_info(exchange, symbol);
    if (info) {
        return info->normalize_qty(qty);
    }
    return qty;
}

double SymbolMaster::normalize_price(Exchange exchange, const std::string& symbol, double price) const {
    auto info = get_info(exchange, symbol);
    if (info) {
        return info->normalize_price(price);
    }
    return price;
}

std::pair<bool, std::string> SymbolMaster::validate_order(
    Exchange exchange,
    const std::string& symbol,
    double qty,
    double price
) const {
    auto info = get_info(exchange, symbol);
    if (!info) {
        return {false, "Symbol not found"};
    }

    if (!info->trading_enabled) {
        return {false, "Trading disabled for this symbol"};
    }

    if (!info->is_valid_qty(qty)) {
        std::ostringstream oss;
        oss << "Invalid quantity: " << qty
            << " (min=" << info->min_qty << ", max=" << info->max_qty << ")";
        return {false, oss.str()};
    }

    if (!info->is_valid_price(price)) {
        std::ostringstream oss;
        oss << "Invalid price: " << price
            << " (min=" << info->min_price << ", max=" << info->max_price << ")";
        return {false, oss.str()};
    }

    if (!info->is_valid_notional(qty, price)) {
        std::ostringstream oss;
        oss << "Notional too small: " << (qty * price)
            << " (min=" << info->min_notional << ")";
        return {false, oss.str()};
    }

    return {true, ""};
}

// =============================================================================
// 기본값 설정
// =============================================================================
void SymbolMaster::init_xrp_defaults() {
    auto now = std::chrono::system_clock::now();

    // Upbit XRP/KRW
    {
        SymbolInfo info;
        info.base = "XRP";
        info.quote = "KRW";
        info.unified = "XRP/KRW";
        info.native = "KRW-XRP";
        info.exchange = Exchange::Upbit;
        info.min_qty = 1.0;
        info.qty_step = 1.0;
        info.qty_precision = 0;
        info.price_step = 1.0;
        info.price_precision = 0;
        info.min_notional = 5000.0;  // 5,000 KRW
        info.updated_at = now;
        register_symbol(info);
    }

    // Bithumb XRP/KRW
    {
        SymbolInfo info;
        info.base = "XRP";
        info.quote = "KRW";
        info.unified = "XRP/KRW";
        info.native = "XRP_KRW";
        info.exchange = Exchange::Bithumb;
        info.min_qty = 1.0;
        info.qty_step = 1.0;
        info.qty_precision = 0;
        info.price_step = 1.0;
        info.price_precision = 0;
        info.min_notional = 1000.0;  // 1,000 KRW
        info.updated_at = now;
        register_symbol(info);
    }

    // Binance XRP/USDT
    {
        SymbolInfo info;
        info.base = "XRP";
        info.quote = "USDT";
        info.unified = "XRP/USDT";
        info.native = "XRPUSDT";
        info.exchange = Exchange::Binance;
        info.min_qty = 1.0;
        info.qty_step = 0.1;
        info.qty_precision = 1;
        info.price_step = 0.0001;
        info.price_precision = 4;
        info.min_notional = 5.0;  // 5 USDT
        info.updated_at = now;
        register_symbol(info);
    }

    // MEXC XRP/USDT
    {
        SymbolInfo info;
        info.base = "XRP";
        info.quote = "USDT";
        info.unified = "XRP/USDT";
        info.native = "XRPUSDT";
        info.exchange = Exchange::MEXC;
        info.min_qty = 1.0;
        info.qty_step = 0.01;
        info.qty_precision = 2;
        info.price_step = 0.0001;
        info.price_precision = 4;
        info.min_notional = 5.0;  // 5 USDT
        info.updated_at = now;
        register_symbol(info);
    }
}

void SymbolMaster::update_from_exchange(Exchange exchange, const std::vector<SymbolInfo>& symbols) {
    std::unique_lock lock(mutex_);

    for (const auto& info : symbols) {
        std::string key = make_key(exchange, info.unified);
        symbols_[key] = info;

        std::string native_key = std::to_string(static_cast<int>(exchange)) + ":" + info.native;
        native_to_unified_[native_key] = info.unified;
    }
}

// =============================================================================
// 설정 파일
// =============================================================================
Result<void> SymbolMaster::save_to_file(const std::string& path) const {
    std::shared_lock lock(mutex_);

    std::ofstream file(path);
    if (!file) {
        return Error(ErrorCode::FileError, "Failed to open file for writing: " + path);
    }

    file << "# Symbol Master Configuration\n\n";

    for (const auto& [key, info] : symbols_) {
        file << "[[symbol]]\n";
        file << "exchange = \"" << exchange_name(info.exchange) << "\"\n";
        file << "base = \"" << info.base << "\"\n";
        file << "quote = \"" << info.quote << "\"\n";
        file << "native = \"" << info.native << "\"\n";
        file << "min_qty = " << info.min_qty << "\n";
        file << "qty_step = " << info.qty_step << "\n";
        file << "qty_precision = " << info.qty_precision << "\n";
        file << "price_step = " << info.price_step << "\n";
        file << "price_precision = " << info.price_precision << "\n";
        file << "min_notional = " << info.min_notional << "\n";
        file << "\n";
    }

    return {};
}

Result<void> SymbolMaster::load_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return Error(ErrorCode::FileError, "Failed to open file for reading: " + path);
    }

    // 간단한 파서 (실제로는 TOML/YAML 라이브러리 사용 권장)
    // 현재는 기본값 사용
    return {};
}

// =============================================================================
// 통계
// =============================================================================
size_t SymbolMaster::count() const {
    std::shared_lock lock(mutex_);
    return symbols_.size();
}

size_t SymbolMaster::count(Exchange exchange) const {
    std::shared_lock lock(mutex_);

    size_t cnt = 0;
    for (const auto& [key, info] : symbols_) {
        if (info.exchange == exchange) {
            ++cnt;
        }
    }

    return cnt;
}

// =============================================================================
// 내부 함수
// =============================================================================
std::string SymbolMaster::make_key(Exchange exchange, const std::string& unified) {
    return std::to_string(static_cast<int>(exchange)) + ":" + unified;
}

}  // namespace arbitrage
