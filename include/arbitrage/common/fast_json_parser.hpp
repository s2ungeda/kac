#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/error.hpp"
#include "arbitrage/common/json.hpp"

#include <string>
#include <string_view>
#include <optional>

namespace arbitrage {

/**
 * Fast JSON Parser
 *
 * 현재: nlohmann/json 기반
 * 향후: simdjson 지원 예정 (SIMD 가속)
 *
 * 특징:
 * - 스레드 로컬 파서로 할당 최소화
 * - 에러 처리 통합
 * - 거래소별 파싱 함수 제공
 */
class FastJsonParser {
public:
    FastJsonParser() = default;

    /**
     * JSON 문자열 파싱
     */
    Result<json> parse(std::string_view input) noexcept {
        try {
            return Ok(json::parse(input));
        } catch (const json::parse_error& e) {
            return Err<json>(ErrorCode::ParseError, e.what());
        } catch (const std::exception& e) {
            return Err<json>(ErrorCode::ParseError, e.what());
        }
    }

    /**
     * JSON 문자열 파싱 (예외 없는 버전)
     */
    std::optional<json> parse_opt(std::string_view input) noexcept {
        try {
            return json::parse(input);
        } catch (...) {
            return std::nullopt;
        }
    }
};

/**
 * 스레드 로컬 파서 (재사용으로 할당 최소화)
 */
inline FastJsonParser& thread_local_parser() {
    thread_local FastJsonParser parser;
    return parser;
}


// =============================================================================
// 거래소별 Ticker 파싱 함수
// =============================================================================

/**
 * Upbit Ticker 파싱
 *
 * 입력 예시:
 * {
 *   "type": "ticker",
 *   "code": "KRW-XRP",
 *   "trade_price": 2700.0,
 *   "trade_volume": 100.5,
 *   "acc_trade_volume_24h": 1000000.0,
 *   "timestamp": 1704067200000
 * }
 */
inline Result<Ticker> parse_upbit_ticker(std::string_view input) noexcept {
    auto result = thread_local_parser().parse(input);
    if (!result) {
        return Err<Ticker>(result.error());
    }

    try {
        const auto& doc = result.value();

        Ticker ticker;
        ticker.exchange = Exchange::Upbit;

        // code에서 심볼 추출 (KRW-XRP -> XRP)
        std::string code = doc.value("code", "");
        auto pos = code.find('-');
        ticker.symbol = (pos != std::string::npos) ? code.substr(pos + 1) : code;

        ticker.price = doc.value("trade_price", 0.0);
        ticker.volume_24h = doc.value("acc_trade_volume_24h", 0.0);

        // bid/ask (있으면)
        ticker.bid = doc.value("best_bid_price", 0.0);
        ticker.ask = doc.value("best_ask_price", 0.0);

        // timestamp
        if (doc.contains("timestamp")) {
            auto ts_ms = doc["timestamp"].get<int64_t>();
            ticker.timestamp = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(ts_ms));
        }

        return Ok(std::move(ticker));
    } catch (const std::exception& e) {
        return Err<Ticker>(ErrorCode::ParseError, e.what());
    }
}

/**
 * Upbit Trade 파싱
 *
 * 입력 예시:
 * {
 *   "type": "trade",
 *   "code": "KRW-XRP",
 *   "trade_price": 2700.0,
 *   "trade_volume": 100.5,
 *   "timestamp": 1704067200000
 * }
 */
inline Result<Ticker> parse_upbit_trade(std::string_view input) noexcept {
    return parse_upbit_ticker(input);  // 같은 형식
}

/**
 * Binance Ticker 파싱 (24hr ticker)
 *
 * 입력 예시:
 * {
 *   "e": "24hrTicker",
 *   "s": "XRPUSDT",
 *   "c": "2.1234",
 *   "v": "1000000.00",
 *   "b": "2.1230",
 *   "a": "2.1240"
 * }
 */
inline Result<Ticker> parse_binance_ticker(std::string_view input) noexcept {
    auto result = thread_local_parser().parse(input);
    if (!result) {
        return Err<Ticker>(result.error());
    }

    try {
        const auto& doc = result.value();

        Ticker ticker;
        ticker.exchange = Exchange::Binance;

        ticker.symbol = doc.value("s", "");

        // 가격은 문자열로 오는 경우가 많음
        if (doc.contains("c")) {
            auto& c = doc["c"];
            ticker.price = c.is_string() ? std::stod(c.get<std::string>()) : c.get<double>();
        }

        // 거래량
        if (doc.contains("v")) {
            auto& v = doc["v"];
            ticker.volume_24h = v.is_string() ? std::stod(v.get<std::string>()) : v.get<double>();
        }

        // bid/ask
        if (doc.contains("b")) {
            auto& b = doc["b"];
            ticker.bid = b.is_string() ? std::stod(b.get<std::string>()) : b.get<double>();
        }
        if (doc.contains("a")) {
            auto& a = doc["a"];
            ticker.ask = a.is_string() ? std::stod(a.get<std::string>()) : a.get<double>();
        }

        ticker.timestamp = std::chrono::system_clock::now();

        return Ok(std::move(ticker));
    } catch (const std::exception& e) {
        return Err<Ticker>(ErrorCode::ParseError, e.what());
    }
}

/**
 * Binance aggTrade 파싱
 *
 * 입력 예시:
 * {
 *   "e": "aggTrade",
 *   "s": "XRPUSDT",
 *   "p": "2.1234",
 *   "q": "100.5",
 *   "T": 1704067200000
 * }
 */
inline Result<Ticker> parse_binance_agg_trade(std::string_view input) noexcept {
    auto result = thread_local_parser().parse(input);
    if (!result) {
        return Err<Ticker>(result.error());
    }

    try {
        const auto& doc = result.value();

        Ticker ticker;
        ticker.exchange = Exchange::Binance;

        ticker.symbol = doc.value("s", "");

        // 가격
        if (doc.contains("p")) {
            auto& p = doc["p"];
            ticker.price = p.is_string() ? std::stod(p.get<std::string>()) : p.get<double>();
        }

        // timestamp
        if (doc.contains("T")) {
            auto ts_ms = doc["T"].get<int64_t>();
            ticker.timestamp = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(ts_ms));
        } else {
            ticker.timestamp = std::chrono::system_clock::now();
        }

        return Ok(std::move(ticker));
    } catch (const std::exception& e) {
        return Err<Ticker>(ErrorCode::ParseError, e.what());
    }
}

/**
 * Bithumb Ticker 파싱
 *
 * 입력 예시:
 * {
 *   "type": "ticker",
 *   "content": {
 *     "symbol": "XRP_KRW",
 *     "closePrice": "2700",
 *     "volume": "1000000"
 *   }
 * }
 */
inline Result<Ticker> parse_bithumb_ticker(std::string_view input) noexcept {
    auto result = thread_local_parser().parse(input);
    if (!result) {
        return Err<Ticker>(result.error());
    }

    try {
        const auto& doc = result.value();

        Ticker ticker;
        ticker.exchange = Exchange::Bithumb;

        // content 객체 안에 데이터가 있음
        const auto& content = doc.contains("content") ? doc["content"] : doc;

        // symbol (XRP_KRW -> XRP)
        std::string symbol = content.value("symbol", "");
        auto pos = symbol.find('_');
        ticker.symbol = (pos != std::string::npos) ? symbol.substr(0, pos) : symbol;

        // 가격
        if (content.contains("closePrice")) {
            auto& p = content["closePrice"];
            ticker.price = p.is_string() ? std::stod(p.get<std::string>()) : p.get<double>();
        }

        // 거래량
        if (content.contains("volume")) {
            auto& v = content["volume"];
            ticker.volume_24h = v.is_string() ? std::stod(v.get<std::string>()) : v.get<double>();
        }

        ticker.timestamp = std::chrono::system_clock::now();

        return Ok(std::move(ticker));
    } catch (const std::exception& e) {
        return Err<Ticker>(ErrorCode::ParseError, e.what());
    }
}

/**
 * Bithumb Trade (transaction) 파싱
 *
 * 입력 예시:
 * {
 *   "type": "transaction",
 *   "content": {
 *     "list": [
 *       {"symbol": "XRP_KRW", "contPrice": "2700", "contQty": "100.5"}
 *     ]
 *   }
 * }
 */
inline Result<Ticker> parse_bithumb_trade(std::string_view input) noexcept {
    auto result = thread_local_parser().parse(input);
    if (!result) {
        return Err<Ticker>(result.error());
    }

    try {
        const auto& doc = result.value();

        Ticker ticker;
        ticker.exchange = Exchange::Bithumb;

        // content.list[0]에서 데이터 추출
        if (doc.contains("content") && doc["content"].contains("list")) {
            const auto& list = doc["content"]["list"];
            if (!list.empty()) {
                const auto& item = list[0];

                std::string symbol = item.value("symbol", "");
                auto pos = symbol.find('_');
                ticker.symbol = (pos != std::string::npos) ? symbol.substr(0, pos) : symbol;

                if (item.contains("contPrice")) {
                    auto& p = item["contPrice"];
                    ticker.price = p.is_string() ? std::stod(p.get<std::string>()) : p.get<double>();
                }
            }
        }

        ticker.timestamp = std::chrono::system_clock::now();

        return Ok(std::move(ticker));
    } catch (const std::exception& e) {
        return Err<Ticker>(ErrorCode::ParseError, e.what());
    }
}

/**
 * MEXC Ticker 파싱
 *
 * 입력 예시:
 * {
 *   "symbol": "XRP_USDT",
 *   "data": {
 *     "lastPrice": 2.1234,
 *     "volume24": 1000000.0
 *   }
 * }
 */
inline Result<Ticker> parse_mexc_ticker(std::string_view input) noexcept {
    auto result = thread_local_parser().parse(input);
    if (!result) {
        return Err<Ticker>(result.error());
    }

    try {
        const auto& doc = result.value();

        Ticker ticker;
        ticker.exchange = Exchange::MEXC;

        ticker.symbol = doc.value("symbol", "");

        // data 객체 안에 있을 수도 있음
        const auto& data = doc.contains("data") ? doc["data"] : doc;

        if (data.contains("lastPrice")) {
            ticker.price = data["lastPrice"].get<double>();
        } else if (data.contains("p")) {
            auto& p = data["p"];
            ticker.price = p.is_string() ? std::stod(p.get<std::string>()) : p.get<double>();
        }

        if (data.contains("volume24")) {
            ticker.volume_24h = data["volume24"].get<double>();
        }

        ticker.timestamp = std::chrono::system_clock::now();

        return Ok(std::move(ticker));
    } catch (const std::exception& e) {
        return Err<Ticker>(ErrorCode::ParseError, e.what());
    }
}

/**
 * MEXC Deal (Trade) 파싱
 *
 * 입력 예시:
 * {
 *   "symbol": "XRP_USDT",
 *   "data": {
 *     "deals": [
 *       {"p": 2.1234, "v": 100.5, "t": 1704067200}
 *     ]
 *   }
 * }
 */
inline Result<Ticker> parse_mexc_deal(std::string_view input) noexcept {
    auto result = thread_local_parser().parse(input);
    if (!result) {
        return Err<Ticker>(result.error());
    }

    try {
        const auto& doc = result.value();

        Ticker ticker;
        ticker.exchange = Exchange::MEXC;

        ticker.symbol = doc.value("symbol", "");

        // data.deals[0] 또는 직접 deals
        const auto& data = doc.contains("data") ? doc["data"] : doc;

        if (data.contains("deals") && !data["deals"].empty()) {
            const auto& deal = data["deals"][0];

            if (deal.contains("p")) {
                auto& p = deal["p"];
                ticker.price = p.is_number() ? p.get<double>() : std::stod(p.get<std::string>());
            }

            if (deal.contains("t")) {
                auto ts = deal["t"].get<int64_t>();
                // MEXC는 초 단위일 수 있음
                if (ts < 10000000000LL) ts *= 1000;  // 초 -> 밀리초
                ticker.timestamp = std::chrono::system_clock::time_point(
                    std::chrono::milliseconds(ts));
            }
        }

        if (ticker.timestamp == TimePoint{}) {
            ticker.timestamp = std::chrono::system_clock::now();
        }

        return Ok(std::move(ticker));
    } catch (const std::exception& e) {
        return Err<Ticker>(ErrorCode::ParseError, e.what());
    }
}


// =============================================================================
// 통합 파싱 함수
// =============================================================================

/**
 * 거래소별 자동 파싱
 */
inline Result<Ticker> parse_ticker(Exchange ex, std::string_view input) noexcept {
    switch (ex) {
        case Exchange::Upbit:
            return parse_upbit_ticker(input);
        case Exchange::Binance:
            return parse_binance_ticker(input);
        case Exchange::Bithumb:
            return parse_bithumb_ticker(input);
        case Exchange::MEXC:
            return parse_mexc_ticker(input);
        default:
            return Err<Ticker>(ErrorCode::InvalidRequest, "Unknown exchange");
    }
}

/**
 * 거래소별 Trade 자동 파싱
 */
inline Result<Ticker> parse_trade(Exchange ex, std::string_view input) noexcept {
    switch (ex) {
        case Exchange::Upbit:
            return parse_upbit_trade(input);
        case Exchange::Binance:
            return parse_binance_agg_trade(input);
        case Exchange::Bithumb:
            return parse_bithumb_trade(input);
        case Exchange::MEXC:
            return parse_mexc_deal(input);
        default:
            return Err<Ticker>(ErrorCode::InvalidRequest, "Unknown exchange");
    }
}

}  // namespace arbitrage
