/**
 * Rate Limiter & Fast JSON Parser Test
 *
 * Token Bucket Rate Limiter 및 거래소별 JSON 파서 테스트
 */

#include "arbitrage/common/rate_limiter.hpp"
#include "arbitrage/common/fast_json_parser.hpp"

#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <iomanip>
#include <atomic>

using namespace arbitrage;
using namespace std::chrono;

// =============================================================================
// Rate Limiter 테스트
// =============================================================================

void test_token_bucket() {
    std::cout << "\n=== Token Bucket Rate Limiter Test ===\n";

    // 초당 10개, 버스트 5
    TokenBucketRateLimiter limiter(10.0, 5);

    std::cout << "Rate: " << limiter.rate() << "/s, Burst: " << limiter.burst() << "\n";
    std::cout << "Initial tokens: " << limiter.tokens() << "\n\n";

    // 버스트 테스트 (5개 즉시 획득 가능)
    int acquired = 0;
    for (int i = 0; i < 10; ++i) {
        if (limiter.try_acquire()) {
            ++acquired;
        }
    }
    std::cout << "Burst test: acquired " << acquired << "/10 immediately\n";
    std::cout << "Remaining tokens: " << limiter.tokens() << "\n\n";

    // 리필 대기
    std::cout << "Waiting 500ms for refill...\n";
    std::this_thread::sleep_for(milliseconds(500));
    std::cout << "Tokens after 500ms: " << limiter.tokens() << " (expected ~5)\n\n";

    // 블로킹 획득 테스트
    std::cout << "Blocking acquire test (5 tokens):\n";
    auto start = steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        limiter.acquire();
    }
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
    std::cout << "Acquired 5 tokens in " << elapsed << "ms\n";
}

void test_rate_limit_manager() {
    std::cout << "\n=== Rate Limit Manager Test ===\n";

    auto& manager = rate_limits();

    // 거래소별 토큰 확인
    std::cout << "\nInitial tokens:\n";
    for (int i = 0; i < static_cast<int>(Exchange::Count); ++i) {
        auto ex = static_cast<Exchange>(i);
        std::cout << "  " << exchange_name(ex) << ": "
                  << "Order=" << manager.tokens(ex, ApiType::Order) << ", "
                  << "Query=" << manager.tokens(ex, ApiType::Query) << "\n";
    }

    // 업비트 주문 Rate Limit 테스트
    std::cout << "\nUpbit Order API test (rate=8/s, burst=10):\n";
    int count = 0;
    auto start = steady_clock::now();

    while (duration_cast<seconds>(steady_clock::now() - start).count() < 1) {
        if (manager.try_acquire(Exchange::Upbit, ApiType::Order)) {
            ++count;
        }
        std::this_thread::sleep_for(microseconds(100));
    }

    std::cout << "  Acquired " << count << " tokens in 1 second\n";
}

void test_concurrent_rate_limit() {
    std::cout << "\n=== Concurrent Rate Limit Test ===\n";

    TokenBucketRateLimiter limiter(100.0, 50);  // 초당 100회

    std::atomic<int> total_acquired{0};
    constexpr int num_threads = 4;
    constexpr int per_thread = 50;

    std::vector<std::thread> threads;
    auto start = steady_clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < per_thread; ++i) {
                limiter.acquire();
                ++total_acquired;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
    std::cout << "  " << num_threads << " threads acquired "
              << total_acquired << " tokens in " << elapsed << "ms\n";
    std::cout << "  Rate: " << (total_acquired * 1000.0 / elapsed) << "/s\n";
}

// =============================================================================
// JSON Parser 테스트
// =============================================================================

void test_upbit_parser() {
    std::cout << "\n=== Upbit Parser Test ===\n";

    const char* json = R"({
        "type": "ticker",
        "code": "KRW-XRP",
        "trade_price": 2710.0,
        "acc_trade_volume_24h": 1500000.5,
        "best_bid_price": 2709.0,
        "best_ask_price": 2711.0,
        "timestamp": 1704067200000
    })";

    auto result = parse_upbit_ticker(json);
    if (result) {
        auto& t = result.value();
        std::cout << "  Exchange: " << exchange_name(t.exchange) << "\n";
        std::cout << "  Symbol: " << t.symbol << "\n";
        std::cout << "  Price: " << t.price << " KRW\n";
        std::cout << "  Bid/Ask: " << t.bid << " / " << t.ask << "\n";
        std::cout << "  Volume 24h: " << t.volume_24h << "\n";
    } else {
        std::cout << "  Parse error: " << result.error().message << "\n";
    }
}

void test_binance_parser() {
    std::cout << "\n=== Binance Parser Test ===\n";

    // aggTrade 형식
    const char* json = R"({
        "e": "aggTrade",
        "s": "XRPUSDT",
        "p": "1.8250",
        "q": "1000.5",
        "T": 1704067200000
    })";

    auto result = parse_binance_agg_trade(json);
    if (result) {
        auto& t = result.value();
        std::cout << "  Exchange: " << exchange_name(t.exchange) << "\n";
        std::cout << "  Symbol: " << t.symbol << "\n";
        std::cout << "  Price: " << t.price << " USDT\n";
    } else {
        std::cout << "  Parse error: " << result.error().message << "\n";
    }
}

void test_bithumb_parser() {
    std::cout << "\n=== Bithumb Parser Test ===\n";

    const char* json = R"({
        "type": "transaction",
        "content": {
            "list": [
                {"symbol": "XRP_KRW", "contPrice": "2708", "contQty": "500.25"}
            ]
        }
    })";

    auto result = parse_bithumb_trade(json);
    if (result) {
        auto& t = result.value();
        std::cout << "  Exchange: " << exchange_name(t.exchange) << "\n";
        std::cout << "  Symbol: " << t.symbol << "\n";
        std::cout << "  Price: " << t.price << " KRW\n";
    } else {
        std::cout << "  Parse error: " << result.error().message << "\n";
    }
}

void test_mexc_parser() {
    std::cout << "\n=== MEXC Parser Test ===\n";

    const char* json = R"({
        "symbol": "XRP_USDT",
        "data": {
            "deals": [
                {"p": 1.8245, "v": 15000, "t": 1704067200}
            ]
        }
    })";

    auto result = parse_mexc_deal(json);
    if (result) {
        auto& t = result.value();
        std::cout << "  Exchange: " << exchange_name(t.exchange) << "\n";
        std::cout << "  Symbol: " << t.symbol << "\n";
        std::cout << "  Price: " << t.price << " USDT\n";
    } else {
        std::cout << "  Parse error: " << result.error().message << "\n";
    }
}

void test_json_benchmark() {
    std::cout << "\n=== JSON Parser Benchmark ===\n";

    const char* json = R"({
        "type": "ticker",
        "code": "KRW-XRP",
        "trade_price": 2710.0,
        "acc_trade_volume_24h": 1500000.5,
        "timestamp": 1704067200000
    })";

    constexpr int iterations = 100000;

    auto start = steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto result = parse_upbit_ticker(json);
        if (!result) {
            std::cout << "Parse failed!\n";
            return;
        }
    }
    auto elapsed = duration_cast<microseconds>(steady_clock::now() - start).count();

    double per_parse_ns = (elapsed * 1000.0) / iterations;
    double parses_per_sec = iterations * 1000000.0 / elapsed;

    std::cout << "  Iterations: " << iterations << "\n";
    std::cout << "  Total time: " << elapsed / 1000.0 << " ms\n";
    std::cout << "  Per parse: " << std::fixed << std::setprecision(1)
              << per_parse_ns << " ns\n";
    std::cout << "  Throughput: " << std::setprecision(0)
              << parses_per_sec << " parses/s\n";
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "==========================================\n";
    std::cout << " Rate Limiter & JSON Parser Test\n";
    std::cout << "==========================================\n";

    // Rate Limiter 테스트
    test_token_bucket();
    test_rate_limit_manager();
    test_concurrent_rate_limit();

    // JSON Parser 테스트
    test_upbit_parser();
    test_binance_parser();
    test_bithumb_parser();
    test_mexc_parser();
    test_json_benchmark();

    std::cout << "\n=== All Tests Completed ===\n";
    return 0;
}
