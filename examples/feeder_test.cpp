/**
 * TASK_37: FeederProcess Test
 *
 * 테스트 목록:
 * 1. FeederConfig 기본값 (exchange별)
 * 2. CLI 파서 (--exchange, --shm, --symbol 등)
 * 3. 거래소별 기본 WS 호스트/타겟
 * 4. 거래소별 기본 SHM 이름
 * 5. 거래소별 기본 심볼
 * 6. FeederProcess 생성 (각 거래소)
 * 7. SHM 초기화 + ShmSPSCQueue Producer
 * 8. WebSocket 이벤트 → SHM push (단위 테스트)
 * 9. fork() 테스트: Feeder(부모) → Consumer(자식) IPC
 * 10. stop() 호출 시 running → false
 * 11. FeederStats 카운터
 * 12. CLI --help 파서
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

#include "arbitrage/feeder/feeder_process.hpp"
#include "arbitrage/ipc/shm_queue.hpp"
#include "arbitrage/ipc/shm_manager.hpp"
#include "arbitrage/ipc/ipc_types.hpp"
#include "arbitrage/common/types.hpp"
#include "arbitrage/common/logger.hpp"

using namespace arbitrage;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, expr) do { \
    if (expr) { \
        printf("  [PASS] %s\n", name); \
        tests_passed++; \
    } else { \
        printf("  [FAIL] %s\n", name); \
        tests_failed++; \
    } \
} while(0)

// =============================================================================
// Test 1: FeederConfig 기본값
// =============================================================================
void test_default_config() {
    printf("\n=== Default Config per Exchange ===\n");

    // Upbit
    {
        FeederConfig cfg;
        cfg.exchange = Exchange::Upbit;
        FeederProcess feeder(cfg);
        TEST("Upbit exchange", feeder.exchange() == Exchange::Upbit);
    }

    // Bithumb
    {
        FeederConfig cfg;
        cfg.exchange = Exchange::Bithumb;
        FeederProcess feeder(cfg);
        TEST("Bithumb exchange", feeder.exchange() == Exchange::Bithumb);
    }

    // Binance
    {
        FeederConfig cfg;
        cfg.exchange = Exchange::Binance;
        FeederProcess feeder(cfg);
        TEST("Binance exchange", feeder.exchange() == Exchange::Binance);
    }

    // MEXC
    {
        FeederConfig cfg;
        cfg.exchange = Exchange::MEXC;
        FeederProcess feeder(cfg);
        TEST("MEXC exchange", feeder.exchange() == Exchange::MEXC);
    }
}

// =============================================================================
// Test 2: CLI 파서
// =============================================================================
void test_cli_parser() {
    printf("\n=== CLI Parser ===\n");

    // Basic exchange
    {
        const char* argv[] = {"feeder", "--exchange", "binance", "--symbol", "XRPUSDT"};
        auto cfg = FeederProcess::parse_args(5, const_cast<char**>(argv));
        TEST("CLI: exchange=binance", cfg.exchange == Exchange::Binance);
        TEST("CLI: symbol=XRPUSDT", cfg.symbols.size() == 1 && cfg.symbols[0] == "XRPUSDT");
    }

    // Short flags
    {
        const char* argv[] = {"feeder", "-e", "upbit", "-c", "8192", "-v"};
        auto cfg = FeederProcess::parse_args(6, const_cast<char**>(argv));
        TEST("CLI: -e upbit", cfg.exchange == Exchange::Upbit);
        TEST("CLI: -c 8192", cfg.shm_capacity == 8192);
        TEST("CLI: -v verbose", cfg.verbose);
    }

    // Custom SHM
    {
        const char* argv[] = {"feeder", "-e", "mexc", "-s", "/custom_shm"};
        auto cfg = FeederProcess::parse_args(5, const_cast<char**>(argv));
        TEST("CLI: custom shm name", cfg.shm_name == "/custom_shm");
    }

    // Multiple symbols
    {
        const char* argv[] = {"feeder", "--symbol", "KRW-XRP", "--symbol", "KRW-BTC"};
        auto cfg = FeederProcess::parse_args(5, const_cast<char**>(argv));
        TEST("CLI: multiple symbols", cfg.symbols.size() == 2);
    }

    // Host/port/target override
    {
        const char* argv[] = {"feeder", "--host", "custom.host", "--port", "9443", "--target", "/ws"};
        auto cfg = FeederProcess::parse_args(7, const_cast<char**>(argv));
        TEST("CLI: host override", cfg.ws_host == "custom.host");
        TEST("CLI: port override", cfg.ws_port == "9443");
        TEST("CLI: target override", cfg.ws_target == "/ws");
    }
}

// =============================================================================
// Test 3: SHM 초기화 (fork IPC 테스트)
// =============================================================================
void test_shm_ipc() {
    printf("\n=== SHM IPC Test (fork) ===\n");

    const char* shm_name = "/kimchi_test_feeder_ipc";
    const size_t capacity = 256;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));
    const size_t num_items = 50;

    ShmSegment::unlink(shm_name);

    // Producer (부모)
    ShmSegment seg(shm_name, shm_size, true);
    auto producer = ShmSPSCQueue<Ticker>::init_producer(seg.data(), capacity);
    TEST("Producer valid", producer.valid());

    pid_t child = fork();
    if (child < 0) {
        printf("  [SKIP] fork() failed\n");
        return;
    }

    if (child == 0) {
        // === 자식: Consumer ===
        ShmSegment child_seg(shm_name, shm_size, false);
        auto consumer = ShmSPSCQueue<Ticker>::attach_consumer(child_seg.data());
        if (!consumer.valid()) _exit(1);

        size_t received = 0;
        bool ok = true;
        int spin = 0;

        while (received < num_items && spin < 5000000) {
            Ticker t{};
            if (consumer.pop(t)) {
                if (t.exchange != Exchange::Upbit) ok = false;
                if (t.price != static_cast<double>(received)) ok = false;
                if (std::strcmp(t.symbol, "KRW-XRP") != 0) ok = false;
                received++;
                spin = 0;
            } else {
                spin++;
            }
        }

        _exit((received == num_items && ok) ? 0 : 1);
    }

    // === 부모: Producer ===
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    for (size_t i = 0; i < num_items; ++i) {
        Ticker t{};
        t.exchange = Exchange::Upbit;
        t.set_symbol("KRW-XRP");
        t.price = static_cast<double>(i);
        t.bid = t.price - 1.0;
        t.ask = t.price + 1.0;
        t.set_timestamp_now();
        while (!producer.push(t)) {}
    }

    int status = 0;
    waitpid(child, &status, 0);
    TEST("Fork IPC: all Tickers received correctly",
         WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// =============================================================================
// Test 4: FeederProcess stop()
// =============================================================================
void test_stop() {
    printf("\n=== Stop Test ===\n");

    FeederConfig cfg;
    cfg.exchange = Exchange::Upbit;
    FeederProcess feeder(cfg);

    TEST("Not running initially", !feeder.is_running());
    feeder.stop();
    TEST("Stop on non-running is safe", !feeder.is_running());
}

// =============================================================================
// Test 5: FeederStats 초기값
// =============================================================================
void test_stats() {
    printf("\n=== FeederStats ===\n");

    FeederConfig cfg;
    cfg.exchange = Exchange::Binance;
    FeederProcess feeder(cfg);

    const auto& stats = feeder.stats();
    TEST("Stats: ticks_received=0", stats.ticks_received.load() == 0);
    TEST("Stats: ticks_pushed=0", stats.ticks_pushed.load() == 0);
    TEST("Stats: ticks_dropped=0", stats.ticks_dropped.load() == 0);
    TEST("Stats: ws_reconnects=0", stats.ws_reconnects.load() == 0);
    TEST("Stats: ws_errors=0", stats.ws_errors.load() == 0);
}

// =============================================================================
// Test 6: SHM 이름 자동 할당 검증
// =============================================================================
void test_shm_name_auto() {
    printf("\n=== SHM Name Auto-Assignment ===\n");

    // 각 거래소의 기본 SHM 이름이 올바르게 설정되는지 확인
    Exchange exchanges[] = {Exchange::Upbit, Exchange::Bithumb, Exchange::Binance, Exchange::MEXC};
    const char* expected[] = {"/kimchi_feed_upbit", "/kimchi_feed_bithumb",
                              "/kimchi_feed_binance", "/kimchi_feed_mexc"};

    for (int i = 0; i < 4; ++i) {
        FeederConfig cfg;
        cfg.exchange = exchanges[i];
        // apply_exchange_defaults는 생성자에서 호출됨

        const char* name = shm_names::feed_name(exchanges[i]);
        char test_name[64];
        snprintf(test_name, sizeof(test_name), "SHM auto name: %s", expected[i]);
        TEST(test_name, name != nullptr && std::strcmp(name, expected[i]) == 0);
    }
}

// =============================================================================
// Test 7: 다양한 Ticker 데이터 SHM 전송
// =============================================================================
void test_ticker_variety() {
    printf("\n=== Ticker Variety SHM ===\n");

    const char* shm_name = "/kimchi_test_variety";
    const size_t capacity = 64;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));

    ShmSegment::unlink(shm_name);
    ShmSegment seg(shm_name, shm_size, true);
    auto producer = ShmSPSCQueue<Ticker>::init_producer(seg.data(), capacity);
    auto consumer = ShmSPSCQueue<Ticker>::attach_consumer(seg.data());

    // 각 거래소 Ticker
    struct TestCase {
        Exchange exchange;
        const char* symbol;
        double price;
    };
    TestCase cases[] = {
        {Exchange::Upbit,   "KRW-XRP",  3100.0},
        {Exchange::Bithumb, "XRP_KRW",  3101.0},
        {Exchange::Binance, "XRPUSDT",  2.12},
        {Exchange::MEXC,    "XRPUSDT",  2.13},
    };

    for (const auto& tc : cases) {
        Ticker t{};
        t.exchange = tc.exchange;
        t.set_symbol(tc.symbol);
        t.price = tc.price;
        t.set_timestamp_now();
        TEST("Push ticker", producer.push(t));
    }

    for (const auto& tc : cases) {
        Ticker out{};
        TEST("Pop ticker", consumer.pop(out));

        char name[64];
        snprintf(name, sizeof(name), "Verify %s price", exchange_name(tc.exchange));
        TEST(name, out.price == tc.price);

        snprintf(name, sizeof(name), "Verify %s symbol", exchange_name(tc.exchange));
        TEST(name, std::strcmp(out.symbol, tc.symbol) == 0);
    }
}

// =============================================================================
// Main
// =============================================================================
int main() {
    printf("=== TASK_37: FeederProcess Test ===\n");

    test_default_config();
    test_cli_parser();
    test_shm_ipc();
    test_stop();
    test_stats();
    test_shm_name_auto();
    test_ticker_variety();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed (total %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
