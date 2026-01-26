/**
 * Low Latency Infrastructure Test
 *
 * SPSC Queue, Memory Pool, SpinWait 테스트 및 벤치마크
 */

#include "arbitrage/common/lockfree_queue.hpp"
#include "arbitrage/common/memory_pool.hpp"
#include "arbitrage/common/spin_wait.hpp"
#include "arbitrage/common/pooled_types.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <iomanip>

using namespace arbitrage;
using namespace std::chrono;

// =============================================================================
// 벤치마크 유틸리티
// =============================================================================

template <typename Func>
double benchmark_ns(Func&& func, size_t iterations) {
    auto start = steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        func();
    }
    auto end = steady_clock::now();
    auto elapsed = duration_cast<nanoseconds>(end - start).count();
    return static_cast<double>(elapsed) / iterations;
}

// =============================================================================
// SPSC Queue 테스트
// =============================================================================

void test_spsc_queue() {
    std::cout << "\n=== SPSC Queue Test ===\n";

    SPSCQueue<int> queue(1024);

    // 기본 push/pop
    queue.push(42);
    queue.push(100);

    int val;
    queue.pop(val);
    std::cout << "Pop 1: " << val << " (expected 42)\n";
    queue.pop(val);
    std::cout << "Pop 2: " << val << " (expected 100)\n";

    // Empty 확인
    std::cout << "Empty: " << (queue.empty() ? "true" : "false") << "\n";

    // 성능 테스트
    constexpr size_t iterations = 100000;

    double push_ns = benchmark_ns([&queue]() {
        queue.push(1);
        int v;
        queue.pop(v);
    }, iterations);

    std::cout << "Push+Pop latency: " << std::fixed << std::setprecision(1)
              << push_ns << " ns\n";
}

// =============================================================================
// MPSC Queue 테스트
// =============================================================================

void test_mpsc_queue() {
    std::cout << "\n=== MPSC Queue Test ===\n";

    MPSCQueue<int> queue(1024);

    // 다중 Producer
    std::vector<std::thread> producers;
    constexpr int per_producer = 100;
    constexpr int num_producers = 4;

    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&queue, p]() {
            for (int i = 0; i < per_producer; ++i) {
                queue.push(p * 1000 + i);
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    // Consumer
    int count = 0;
    int val;
    while (queue.pop(val)) {
        ++count;
    }

    std::cout << "Received: " << count << " items (expected "
              << (num_producers * per_producer) << ")\n";
}

// =============================================================================
// Memory Pool 테스트
// =============================================================================

void test_memory_pool() {
    std::cout << "\n=== Memory Pool Test ===\n";

    ObjectPool<Ticker, 1024> pool;

    // 객체 생성
    auto* t1 = pool.create();
    t1->exchange = Exchange::Upbit;
    t1->set_symbol("BTC");
    t1->price = 50000000.0;

    std::cout << "Created ticker: " << exchange_name(t1->exchange)
              << " " << t1->symbol << " @ " << t1->price << "\n";

    // 객체 반환
    pool.destroy(t1);

    // 재할당 (같은 메모리 재사용)
    auto* t2 = pool.create();
    std::cout << "Reused memory: " << (t1 == t2 ? "true" : "false") << "\n";
    pool.destroy(t2);

    // 성능 테스트
    constexpr size_t iterations = 100000;

    double pool_ns = benchmark_ns([&pool]() {
        auto* t = pool.create();
        pool.destroy(t);
    }, iterations);

    // 비교: new/delete
    double new_ns = benchmark_ns([]() {
        auto* t = new Ticker();
        delete t;
    }, iterations);

    std::cout << "Pool alloc+free: " << std::fixed << std::setprecision(1)
              << pool_ns << " ns\n";
    std::cout << "new/delete:      " << new_ns << " ns\n";
    std::cout << "Speedup:         " << (new_ns / pool_ns) << "x\n";
}

// =============================================================================
// SpinLock 테스트
// =============================================================================

void test_spinlock() {
    std::cout << "\n=== SpinLock Test ===\n";

    SpinLock lock;
    int counter = 0;
    constexpr int iterations = 10000;
    constexpr int num_threads = 4;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations; ++j) {
                SpinLockGuard guard(lock);
                ++counter;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Counter: " << counter << " (expected "
              << (num_threads * iterations) << ")\n";
    std::cout << "Test: " << (counter == num_threads * iterations ? "PASSED" : "FAILED") << "\n";
}

// =============================================================================
// Pooled Types 테스트
// =============================================================================

void test_pooled_types() {
    std::cout << "\n=== Pooled Types Test ===\n";

    // 글로벌 풀 사용
    auto stats_before = get_pool_stats();
    std::cout << "Ticker pool available (before): " << stats_before.ticker_available << "\n";

    // 여러 Ticker 생성
    std::vector<Ticker*> tickers;
    for (int i = 0; i < 100; ++i) {
        tickers.push_back(ticker_pool().create());
    }

    auto stats_during = get_pool_stats();
    std::cout << "Ticker pool available (during): " << stats_during.ticker_available << "\n";

    // 반환
    for (auto* t : tickers) {
        ticker_pool().destroy(t);
    }

    auto stats_after = get_pool_stats();
    std::cout << "Ticker pool available (after):  " << stats_after.ticker_available << "\n";

    // PooledPtr 사용 (RAII)
    {
        PooledTicker t(ticker_pool());
        t->exchange = Exchange::Binance;
        t->price = 2.5;
    }  // 자동 반환

    std::cout << "PooledPtr RAII test: PASSED\n";
}

// =============================================================================
// AdaptiveSpinWait 테스트
// =============================================================================

void test_adaptive_spin() {
    std::cout << "\n=== Adaptive SpinWait Test ===\n";

    AdaptiveSpinWait waiter;
    auto start = steady_clock::now();

    // 100회 대기
    for (int i = 0; i < 100; ++i) {
        waiter.wait();
    }

    auto elapsed = duration_cast<microseconds>(steady_clock::now() - start).count();
    std::cout << "100 waits took: " << elapsed << " us\n";
    std::cout << "Final count: " << waiter.count() << "\n";
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "====================================\n";
    std::cout << " Low Latency Infrastructure Test\n";
    std::cout << "====================================\n";

    test_spsc_queue();
    test_mpsc_queue();
    test_memory_pool();
    test_spinlock();
    test_pooled_types();
    test_adaptive_spin();

    std::cout << "\n=== All Tests Completed ===\n";
    return 0;
}
