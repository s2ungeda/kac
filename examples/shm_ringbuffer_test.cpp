/**
 * ShmRingBuffer + ShmLatestValue 단위 테스트
 *
 * 테스트 목록:
 *  [RingBuffer]
 *   1. 기본 push/pop
 *   2. 오버라이트 — capacity 초과 시 오래된 데이터 폐기
 *   3. Consumer catch-up — 뒤처진 consumer 자동 최신 위치 이동
 *   4. pop_latest — 마지막 값만 반환
 *   5. empty/size/close 상태 조회
 *   6. Fork IPC — 프로세스 간 producer/consumer
 *
 *  [LatestValue]
 *   7. 기본 store/load
 *   8. 덮어쓰기 — 항상 최신 값 반환
 *   9. seqlock 일관성 — has_data/sequence
 *  10. load_spin 재시도
 *  11. Fork IPC — 프로세스 간 최신 값 공유
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

#include "arbitrage/common/types.hpp"
#include "arbitrage/ipc/ipc_types.hpp"
#include "arbitrage/ipc/shm_ring_buffer.hpp"
#include "arbitrage/ipc/shm_latest.hpp"
#include "arbitrage/ipc/shm_manager.hpp"

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
// Helper: Ticker 생성
// =============================================================================
static Ticker make_ticker(Exchange ex, double price, int64_t seq) {
    Ticker t{};
    t.exchange = ex;
    t.set_symbol("KRW-XRP");
    t.price = price;
    t.bid = price - 1.0;
    t.ask = price + 1.0;
    t.volume_24h = 100.0;
    t.timestamp_us = seq;
    return t;
}

// =============================================================================
// Test 1: RingBuffer 기본 push/pop
// =============================================================================
void test_ringbuffer_basic() {
    printf("\n=== RingBuffer: Basic push/pop ===\n");

    const char* name = "/kimchi_test_rb_basic";
    const size_t capacity = 64;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmRingBuffer<Ticker>::init_producer(seg.data(), capacity);
    TEST("Producer valid", producer.valid());
    TEST("Initially empty", producer.empty());
    TEST("Initial size 0", producer.size() == 0);

    // push 10개
    for (int i = 0; i < 10; ++i) {
        producer.push(make_ticker(Exchange::Upbit, 3000.0 + i, i));
    }

    // consumer attach & pop
    auto consumer = ShmRingBuffer<Ticker>::attach_consumer(seg.data());
    TEST("Consumer valid", consumer.valid());
    TEST("Size == 10", consumer.size() == 10);

    Ticker out{};
    for (int i = 0; i < 10; ++i) {
        bool ok = consumer.pop(out);
        TEST("Pop success", ok);
        if (ok) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Price[%d] == %.0f", i, 3000.0 + i);
            TEST(buf, out.price == 3000.0 + i);
        }
    }

    TEST("Empty after drain", consumer.empty());
    TEST("Pop on empty returns false", !consumer.pop(out));
}

// =============================================================================
// Test 2: RingBuffer 오버라이트
// =============================================================================
void test_ringbuffer_overwrite() {
    printf("\n=== RingBuffer: Overwrite (old data discarded) ===\n");

    const char* name = "/kimchi_test_rb_overwrite";
    const size_t capacity = 16;  // 작은 용량
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmRingBuffer<Ticker>::init_producer(seg.data(), capacity);

    // capacity * 3 = 48개 push (32개는 덮어씌워짐)
    for (int i = 0; i < 48; ++i) {
        producer.push(make_ticker(Exchange::Upbit, 1000.0 + i, i));
    }

    TEST("total_pushed == 48", producer.total_pushed() == 48);

    // consumer는 최신 데이터만 읽을 수 있어야 함
    auto consumer = ShmRingBuffer<Ticker>::attach_consumer(seg.data());

    Ticker out{};
    int count = 0;
    double first_price = 0;
    double last_price = 0;

    while (consumer.pop(out)) {
        if (count == 0) first_price = out.price;
        last_price = out.price;
        count++;
    }

    TEST("Read count <= capacity", count <= (int)capacity);
    TEST("Read count > 0", count > 0);
    TEST("Last price == 1047", last_price == 1047.0);
    // 오래된 데이터(1000~1032)는 사라지고, 최신 데이터만 남아야 함
    TEST("First price >= 1033", first_price >= 1033.0);

    printf("  (read %d items, first=%.0f, last=%.0f)\n", count, first_price, last_price);
}

// =============================================================================
// Test 3: Consumer catch-up
// =============================================================================
void test_ringbuffer_catchup() {
    printf("\n=== RingBuffer: Consumer catch-up ===\n");

    const char* name = "/kimchi_test_rb_catchup";
    const size_t capacity = 32;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmRingBuffer<Ticker>::init_producer(seg.data(), capacity);
    auto consumer = ShmRingBuffer<Ticker>::attach_consumer(seg.data());

    // consumer가 5개 읽음
    for (int i = 0; i < 5; ++i) {
        producer.push(make_ticker(Exchange::Upbit, 100.0 + i, i));
    }
    Ticker out{};
    for (int i = 0; i < 5; ++i) {
        consumer.pop(out);
    }
    TEST("Consumer caught up to 5", out.price == 104.0);

    // producer가 capacity * 2 = 64개 더 push (consumer는 안 읽음)
    for (int i = 5; i < 69; ++i) {
        producer.push(make_ticker(Exchange::Upbit, 100.0 + i, i));
    }

    // consumer pop → 자동으로 최신 위치로 catch-up
    bool ok = consumer.pop(out);
    TEST("Pop after catch-up succeeds", ok);
    // catch-up 후 오래된 값이 아닌 최신 근처 값이어야 함
    TEST("Catch-up: price >= 137", out.price >= 137.0);  // 69 - 31 = 38번째 이상

    printf("  (catch-up price=%.0f)\n", out.price);
}

// =============================================================================
// Test 4: pop_latest
// =============================================================================
void test_ringbuffer_pop_latest() {
    printf("\n=== RingBuffer: pop_latest ===\n");

    const char* name = "/kimchi_test_rb_latest";
    const size_t capacity = 64;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmRingBuffer<Ticker>::init_producer(seg.data(), capacity);
    auto consumer = ShmRingBuffer<Ticker>::attach_consumer(seg.data());

    // 20개 push
    for (int i = 0; i < 20; ++i) {
        producer.push(make_ticker(Exchange::Upbit, 5000.0 + i, i));
    }

    Ticker out{};
    bool ok = consumer.pop_latest(out);
    TEST("pop_latest succeeds", ok);
    TEST("pop_latest returns last item (5019)", out.price == 5019.0);
    TEST("Queue empty after pop_latest", consumer.empty());
}

// =============================================================================
// Test 5: empty/size/close
// =============================================================================
void test_ringbuffer_state() {
    printf("\n=== RingBuffer: State queries ===\n");

    const char* name = "/kimchi_test_rb_state";
    const size_t capacity = 64;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmRingBuffer<Ticker>::init_producer(seg.data(), capacity);

    TEST("Empty initially", producer.empty());
    TEST("Size 0 initially", producer.size() == 0);
    TEST("Not closed", !producer.is_closed());
    TEST("Capacity == 64", producer.capacity() == 64);

    producer.push(make_ticker(Exchange::Binance, 2.0, 1));
    TEST("Not empty after push", !producer.empty());
    TEST("Size 1 after push", producer.size() == 1);

    producer.close();
    TEST("Closed after close()", producer.is_closed());
}

// =============================================================================
// Test 6: RingBuffer Fork IPC
// =============================================================================
void test_ringbuffer_fork_ipc() {
    printf("\n=== RingBuffer: Fork IPC ===\n");

    const char* name = "/kimchi_test_rb_ipc";
    const size_t capacity = 256;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));
    const size_t num_items = 100;

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmRingBuffer<Ticker>::init_producer(seg.data(), capacity);
    TEST("IPC producer valid", producer.valid());

    pid_t child = fork();
    if (child < 0) {
        printf("  [SKIP] fork() failed\n");
        return;
    }

    if (child == 0) {
        // === 자식: Consumer ===
        ShmSegment child_seg(name, shm_size, false);
        auto consumer = ShmRingBuffer<Ticker>::attach_consumer(child_seg.data());
        if (!consumer.valid()) _exit(1);

        size_t received = 0;
        bool ok = true;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        while (received < num_items &&
               std::chrono::steady_clock::now() < deadline) {
            Ticker t{};
            if (consumer.pop(t)) {
                if (t.price != static_cast<double>(received)) ok = false;
                if (t.exchange != Exchange::Upbit) ok = false;
                received++;
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }

        _exit((received == num_items && ok) ? 0 : 1);
    }

    // === 부모: Producer ===
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    for (size_t i = 0; i < num_items; ++i) {
        producer.push(make_ticker(Exchange::Upbit, static_cast<double>(i), i));
    }

    int status = 0;
    waitpid(child, &status, 0);
    TEST("Fork IPC: all items received correctly",
         WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// =============================================================================
// Test 7: LatestValue 기본 store/load
// =============================================================================
void test_latest_basic() {
    printf("\n=== LatestValue: Basic store/load ===\n");

    const char* name = "/kimchi_test_lv_basic";
    const size_t shm_size = shm_latest_size<OrderBook>();

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmLatestValue<OrderBook>::init_producer(seg.data(), 0);
    TEST("Producer valid", producer.valid());
    TEST("No data initially", !producer.has_data());

    // store
    OrderBook ob{};
    ob.exchange = Exchange::Upbit;
    ob.set_symbol("KRW-XRP");
    ob.add_bid(3100.0, 1000.0);
    ob.add_bid(3099.0, 500.0);
    ob.add_ask(3101.0, 800.0);
    ob.add_ask(3102.0, 600.0);
    ob.timestamp_us = 12345;

    producer.store(ob);
    TEST("Has data after store", producer.has_data());
    TEST("Sequence >= 2", producer.sequence() >= 2);

    // consumer attach & load
    auto consumer = ShmLatestValue<OrderBook>::attach_consumer(seg.data());
    TEST("Consumer valid", consumer.valid());

    OrderBook out{};
    bool ok = consumer.load(out);
    TEST("Load succeeds", ok);
    TEST("Exchange matches", out.exchange == Exchange::Upbit);
    TEST("Symbol matches", std::strcmp(out.symbol, "KRW-XRP") == 0);
    TEST("bid_count == 2", out.bid_count == 2);
    TEST("ask_count == 2", out.ask_count == 2);
    TEST("best_bid == 3100", out.best_bid() == 3100.0);
    TEST("best_ask == 3101", out.best_ask() == 3101.0);
    TEST("bids[1].price == 3099", out.bids[1].price == 3099.0);
    TEST("asks[1].quantity == 600", out.asks[1].quantity == 600.0);
}

// =============================================================================
// Test 8: LatestValue 덮어쓰기
// =============================================================================
void test_latest_overwrite() {
    printf("\n=== LatestValue: Overwrite (always latest) ===\n");

    const char* name = "/kimchi_test_lv_overwrite";
    const size_t shm_size = shm_latest_size<OrderBook>();

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmLatestValue<OrderBook>::init_producer(seg.data(), 0);
    auto consumer = ShmLatestValue<OrderBook>::attach_consumer(seg.data());

    // 100번 덮어쓰기
    for (int i = 0; i < 100; ++i) {
        OrderBook ob{};
        ob.exchange = Exchange::Binance;
        ob.set_symbol("XRPUSDT");
        ob.add_bid(2.10 + i * 0.01, 1000.0 + i);
        ob.add_ask(2.11 + i * 0.01, 2000.0 + i);
        ob.timestamp_us = i;
        producer.store(ob);
    }

    // load는 항상 마지막 값
    OrderBook out{};
    bool ok = consumer.load(out);
    TEST("Load after 100 stores succeeds", ok);
    TEST("Timestamp == 99 (latest)", out.timestamp_us == 99);
    TEST("bid price == 3.09", out.bids[0].price > 3.08 && out.bids[0].price < 3.10);
    TEST("bid quantity == 1099", out.bids[0].quantity == 1099.0);

    // 한 번 더 load — 같은 값
    OrderBook out2{};
    ok = consumer.load(out2);
    TEST("Second load same value", ok && out2.timestamp_us == 99);
}

// =============================================================================
// Test 9: LatestValue seqlock consistency
// =============================================================================
void test_latest_seqlock() {
    printf("\n=== LatestValue: Seqlock consistency ===\n");

    const char* name = "/kimchi_test_lv_seq";
    const size_t shm_size = shm_latest_size<OrderBook>();

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmLatestValue<OrderBook>::init_producer(seg.data(), 0);
    auto consumer = ShmLatestValue<OrderBook>::attach_consumer(seg.data());

    TEST("Sequence 0 initially", consumer.sequence() == 0);
    TEST("has_data false", !consumer.has_data());

    OrderBook ob{};
    ob.exchange = Exchange::Upbit;
    ob.set_symbol("KRW-XRP");
    ob.add_bid(3100.0, 100.0);
    ob.add_ask(3101.0, 200.0);

    producer.store(ob);
    TEST("Sequence == 2 after first store", consumer.sequence() == 2);
    TEST("has_data true", consumer.has_data());

    producer.store(ob);
    TEST("Sequence == 4 after second store", consumer.sequence() == 4);

    producer.store(ob);
    TEST("Sequence == 6 after third store", consumer.sequence() == 6);
    TEST("Sequence is always even", (consumer.sequence() & 1) == 0);
}

// =============================================================================
// Test 10: LatestValue load_spin
// =============================================================================
void test_latest_load_spin() {
    printf("\n=== LatestValue: load_spin ===\n");

    const char* name = "/kimchi_test_lv_spin";
    const size_t shm_size = shm_latest_size<OrderBook>();

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmLatestValue<OrderBook>::init_producer(seg.data(), 0);
    auto consumer = ShmLatestValue<OrderBook>::attach_consumer(seg.data());

    // 데이터 없을 때 load_spin은 실패
    OrderBook out{};
    bool ok = consumer.load_spin(out, 10);
    TEST("load_spin fails with no data", !ok);

    // store 후 성공
    OrderBook ob{};
    ob.exchange = Exchange::MEXC;
    ob.set_symbol("XRPUSDT");
    ob.add_bid(1.40, 5000.0);
    ob.add_ask(1.41, 3000.0);
    producer.store(ob);

    ok = consumer.load_spin(out, 10);
    TEST("load_spin succeeds after store", ok);
    TEST("load_spin value correct", out.exchange == Exchange::MEXC);
    TEST("load_spin bid correct", out.bids[0].price == 1.40);
}

// =============================================================================
// Test 11: LatestValue Fork IPC
// =============================================================================
void test_latest_fork_ipc() {
    printf("\n=== LatestValue: Fork IPC ===\n");

    const char* name = "/kimchi_test_lv_ipc";
    const size_t shm_size = shm_latest_size<OrderBook>();

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmLatestValue<OrderBook>::init_producer(seg.data(), 0);
    TEST("IPC producer valid", producer.valid());

    pid_t child = fork();
    if (child < 0) {
        printf("  [SKIP] fork() failed\n");
        return;
    }

    if (child == 0) {
        // === 자식: Consumer — 최종 값이 올바른지 확인 ===
        ShmSegment child_seg(name, shm_size, false);
        auto consumer = ShmLatestValue<OrderBook>::attach_consumer(child_seg.data());
        if (!consumer.valid()) _exit(1);

        // producer가 100회 store할 때까지 대기
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        OrderBook out{};
        bool got_final = false;

        while (std::chrono::steady_clock::now() < deadline) {
            if (consumer.load(out)) {
                // 마지막 store의 timestamp는 99
                if (out.timestamp_us == 99) {
                    got_final = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        if (!got_final) _exit(1);

        // 값 검증
        bool ok = true;
        if (out.exchange != Exchange::Bithumb) ok = false;
        if (std::strcmp(out.symbol, "KRW-XRP") != 0) ok = false;
        if (out.bid_count != 1 || out.ask_count != 1) ok = false;
        if (out.bids[0].price < 3199.0 || out.bids[0].price > 3200.0) ok = false;

        _exit(ok ? 0 : 1);
    }

    // === 부모: Producer — 100번 store ===
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    for (int i = 0; i < 100; ++i) {
        OrderBook ob{};
        ob.exchange = Exchange::Bithumb;
        ob.set_symbol("KRW-XRP");
        ob.add_bid(3100.0 + i, 1000.0);
        ob.add_ask(3101.0 + i, 2000.0);
        ob.timestamp_us = i;
        producer.store(ob);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    int status = 0;
    waitpid(child, &status, 0);
    TEST("Fork IPC: consumer got final value correctly",
         WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// =============================================================================
// Main
// =============================================================================
int main() {
    printf("=== ShmRingBuffer + ShmLatestValue Unit Tests ===\n");

    // RingBuffer tests
    test_ringbuffer_basic();
    test_ringbuffer_overwrite();
    test_ringbuffer_catchup();
    test_ringbuffer_pop_latest();
    test_ringbuffer_state();
    test_ringbuffer_fork_ipc();

    // LatestValue tests
    test_latest_basic();
    test_latest_overwrite();
    test_latest_seqlock();
    test_latest_load_spin();
    test_latest_fork_ipc();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed (total %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
