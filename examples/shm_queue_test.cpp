/**
 * TASK_36: ShmSPSCQueue Test
 *
 * 테스트 목록:
 * 1. Ticker trivially_copyable 검증
 * 2. ShmQueueHeader 크기 검증
 * 3. ShmAtomicIndex lock-free 검증
 * 4. ShmSegment 생성/해제
 * 5. ShmSPSCQueue init_producer
 * 6. ShmSPSCQueue attach_consumer
 * 7. 단일 프로세스 push/pop
 * 8. 큐 가득 참 (full) 검증
 * 9. 큐 비어있음 (empty) 검증
 * 10. close() 상태 전이
 * 11. producer_pid / consumer_pid
 * 12. 대량 데이터 push/pop (순서 검증)
 * 13. shm_queue_size 계산 검증
 * 14. SHM 이름 규칙 검증
 * 15. 잘못된 magic 검증
 * 16. 잘못된 element_size 검증
 * 17. ShmSegment 이동 생성자
 * 18. fork() 테스트: 부모 write → 자식 read
 * 19. is_producer_alive 검증
 * 20. 성능 벤치마크 (push/pop 지연)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

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
// Test 1: Static assertions
// =============================================================================
void test_static_assertions() {
    printf("\n=== Static Assertions ===\n");

    TEST("Ticker is trivially copyable",
         std::is_trivially_copyable_v<Ticker>);

    TEST("Ticker is 64 bytes",
         sizeof(Ticker) == 64);

    TEST("ShmQueueHeader is 64 bytes",
         sizeof(ShmQueueHeader) == CACHE_LINE_SIZE);

    TEST("ShmAtomicIndex is 64 bytes",
         sizeof(ShmAtomicIndex) == CACHE_LINE_SIZE);

    TEST("atomic<uint64_t> is lock-free",
         std::atomic<uint64_t>::is_always_lock_free);
}

// =============================================================================
// Test 2: SHM 이름 규칙
// =============================================================================
void test_shm_names() {
    printf("\n=== SHM Names ===\n");

    TEST("Upbit feed name",
         std::strcmp(shm_names::feed_name(Exchange::Upbit), "/kimchi_feed_upbit") == 0);
    TEST("Bithumb feed name",
         std::strcmp(shm_names::feed_name(Exchange::Bithumb), "/kimchi_feed_bithumb") == 0);
    TEST("Binance feed name",
         std::strcmp(shm_names::feed_name(Exchange::Binance), "/kimchi_feed_binance") == 0);
    TEST("MEXC feed name",
         std::strcmp(shm_names::feed_name(Exchange::MEXC), "/kimchi_feed_mexc") == 0);
}

// =============================================================================
// Test 3: shm_queue_size 계산
// =============================================================================
void test_queue_size() {
    printf("\n=== Queue Size Calculation ===\n");

    size_t expected = sizeof(ShmQueueHeader) + 2 * sizeof(ShmAtomicIndex)
                      + 4096 * sizeof(Ticker);
    size_t actual = shm_queue_size(4096, sizeof(Ticker));
    TEST("shm_queue_size(4096, Ticker)",
         expected == actual);

    // 64 + 64 + 64 + 4096*64 = 262336 bytes ≈ 256KB
    printf("  Queue size: %zu bytes (%.1f KB)\n", actual, actual / 1024.0);
}

// =============================================================================
// Test 4: ShmSegment 생성/해제
// =============================================================================
void test_shm_segment() {
    printf("\n=== ShmSegment Create/Destroy ===\n");

    const char* name = "/kimchi_test_seg";
    const size_t size = 4096;

    // 사전 정리
    ShmSegment::unlink(name);

    {
        ShmSegment seg(name, size, true);
        TEST("ShmSegment created", seg.valid());
        TEST("ShmSegment size correct", seg.size() == size);
        TEST("ShmSegment data not null", seg.data() != nullptr);
        TEST("ShmSegment name correct", seg.name() == name);

        // 데이터 쓰기/읽기
        auto* p = static_cast<uint32_t*>(seg.data());
        *p = 0xCAFEBABE;
        TEST("ShmSegment write/read", *p == 0xCAFEBABE);
    }
    // 소멸자에서 unlink됨

    // 이동 생성자 테스트
    {
        ShmSegment seg1(name, size, true);
        void* orig_data = seg1.data();
        ShmSegment seg2(std::move(seg1));
        TEST("ShmSegment move: new valid", seg2.valid());
        TEST("ShmSegment move: data preserved", seg2.data() == orig_data);
        TEST("ShmSegment move: old invalidated", !seg1.valid());
    }
}

// =============================================================================
// Test 5: ShmSPSCQueue 단일 프로세스 push/pop
// =============================================================================
void test_single_process() {
    printf("\n=== Single Process Push/Pop ===\n");

    const char* name = "/kimchi_test_spsc";
    const size_t capacity = 64;  // 작은 큐로 테스트
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    // Producer 초기화
    auto producer = ShmSPSCQueue<Ticker>::init_producer(seg.data(), capacity);
    TEST("Producer init valid", producer.valid());
    TEST("Producer capacity", producer.capacity() == capacity);
    TEST("Producer empty", producer.empty());
    TEST("Producer size == 0", producer.size() == 0);
    TEST("Producer PID", producer.producer_pid() == getpid());

    // Consumer 연결
    auto consumer = ShmSPSCQueue<Ticker>::attach_consumer(seg.data());
    TEST("Consumer attach valid", consumer.valid());
    TEST("Consumer capacity", consumer.capacity() == capacity);
    TEST("Consumer PID", consumer.consumer_pid() == getpid());

    // Push 1개
    Ticker t{};
    t.exchange = Exchange::Upbit;
    t.set_symbol("KRW-XRP");
    t.price = 3100.0;
    t.bid = 3099.0;
    t.ask = 3101.0;
    t.volume_24h = 1000000.0;
    t.set_timestamp_now();

    TEST("Push succeeds", producer.push(t));
    TEST("Not empty after push", !consumer.empty());
    TEST("Size == 1 after push", consumer.size() == 1);

    // Pop 1개
    Ticker out{};
    TEST("Pop succeeds", consumer.pop(out));
    TEST("Pop exchange", out.exchange == Exchange::Upbit);
    TEST("Pop symbol", std::strcmp(out.symbol, "KRW-XRP") == 0);
    TEST("Pop price", out.price == 3100.0);
    TEST("Pop bid", out.bid == 3099.0);
    TEST("Pop ask", out.ask == 3101.0);
    TEST("Empty after pop", consumer.empty());

    // Empty pop
    Ticker dummy{};
    TEST("Pop on empty returns false", !consumer.pop(dummy));
}

// =============================================================================
// Test 6: 큐 가득 참
// =============================================================================
void test_queue_full() {
    printf("\n=== Queue Full ===\n");

    const char* name = "/kimchi_test_full";
    const size_t capacity = 8;  // 용량 8 → 최대 7개 저장 가능 (1슬롯 비워둠)
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmSPSCQueue<Ticker>::init_producer(seg.data(), capacity);
    auto consumer = ShmSPSCQueue<Ticker>::attach_consumer(seg.data());

    // capacity - 1 개 push
    size_t pushed = 0;
    for (size_t i = 0; i < capacity; ++i) {
        Ticker t{};
        t.price = static_cast<double>(i);
        if (producer.push(t)) pushed++;
    }
    TEST("Push capacity-1 items", pushed == capacity - 1);

    // 가득 찬 상태에서 push 실패
    Ticker extra{};
    extra.price = 999.0;
    TEST("Push on full returns false", !producer.push(extra));

    // 1개 pop 후 다시 push 가능
    Ticker out{};
    TEST("Pop 1 item", consumer.pop(out));
    TEST("Popped first item", out.price == 0.0);
    TEST("Push after pop succeeds", producer.push(extra));
}

// =============================================================================
// Test 7: Close 상태 전이
// =============================================================================
void test_close() {
    printf("\n=== Close State ===\n");

    const char* name = "/kimchi_test_close";
    const size_t capacity = 16;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmSPSCQueue<Ticker>::init_producer(seg.data(), capacity);
    auto consumer = ShmSPSCQueue<Ticker>::attach_consumer(seg.data());

    TEST("Not closed initially", !consumer.is_closed());

    // push some data before close
    Ticker t{};
    t.price = 42.0;
    producer.push(t);

    producer.close();
    TEST("Closed after close()", consumer.is_closed());

    // 남은 데이터는 여전히 pop 가능
    Ticker out{};
    TEST("Pop after close", consumer.pop(out));
    TEST("Pop data correct after close", out.price == 42.0);
}

// =============================================================================
// Test 8: 대량 데이터 순서 검증
// =============================================================================
void test_bulk_ordering() {
    printf("\n=== Bulk Data Ordering ===\n");

    const char* name = "/kimchi_test_bulk";
    const size_t capacity = 4096;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));
    const size_t count = 10000;

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmSPSCQueue<Ticker>::init_producer(seg.data(), capacity);
    auto consumer = ShmSPSCQueue<Ticker>::attach_consumer(seg.data());

    size_t pushed = 0;
    size_t popped = 0;
    bool order_ok = true;

    for (size_t i = 0; i < count; ++i) {
        // Push
        Ticker t{};
        t.price = static_cast<double>(i);
        t.timestamp_us = static_cast<int64_t>(i);
        while (!producer.push(t)) {
            // Drain some if full
            Ticker out{};
            if (consumer.pop(out)) {
                if (out.timestamp_us != static_cast<int64_t>(popped)) {
                    order_ok = false;
                }
                popped++;
            }
        }
        pushed++;
    }

    // Drain remaining
    Ticker out{};
    while (consumer.pop(out)) {
        if (out.timestamp_us != static_cast<int64_t>(popped)) {
            order_ok = false;
        }
        popped++;
    }

    TEST("All items pushed", pushed == count);
    TEST("All items popped", popped == count);
    TEST("Order preserved", order_ok);
}

// =============================================================================
// Test 9: Invalid attach
// =============================================================================
void test_invalid_attach() {
    printf("\n=== Invalid Attach ===\n");

    // Wrong magic
    alignas(CACHE_LINE_SIZE) char buf[1024]{};
    auto* header = reinterpret_cast<ShmQueueHeader*>(buf);
    header->magic = 0x12345678;  // wrong magic

    auto q = ShmSPSCQueue<Ticker>::attach_consumer(buf);
    TEST("Invalid magic → invalid queue", !q.valid());

    // Wrong element size
    header->magic = SHM_MAGIC;
    header->version = SHM_VERSION;
    header->element_size = 32;  // wrong size (Ticker is 64)
    header->state = static_cast<uint8_t>(ShmQueueState::Ready);

    auto q2 = ShmSPSCQueue<Ticker>::attach_consumer(buf);
    TEST("Wrong element_size → invalid queue", !q2.valid());
}

// =============================================================================
// Test 10: fork() 테스트 — 부모 write → 자식 read
// =============================================================================
void test_fork() {
    printf("\n=== Fork Test: Parent Write → Child Read ===\n");

    const char* name = "/kimchi_test_fork";
    const size_t capacity = 256;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));
    const size_t num_items = 100;

    ShmSegment::unlink(name);

    // 부모가 SHM 생성 + producer 초기화
    ShmSegment seg(name, shm_size, true);
    auto producer = ShmSPSCQueue<Ticker>::init_producer(seg.data(), capacity);

    pid_t child = fork();
    if (child < 0) {
        printf("  [SKIP] fork() failed\n");
        return;
    }

    if (child == 0) {
        // === 자식 프로세스: Consumer ===
        // 자식은 부모가 생성한 SHM을 다시 열어서 attach
        ShmSegment child_seg(name, shm_size, false);
        auto consumer = ShmSPSCQueue<Ticker>::attach_consumer(child_seg.data());

        if (!consumer.valid()) {
            _exit(1);
        }

        size_t received = 0;
        bool order_ok = true;

        // 타임아웃 기반 대기 (spin count 대신 — WSL2 등 느린 환경 대응)
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        while (received < num_items &&
               std::chrono::steady_clock::now() < deadline) {
            Ticker t{};
            if (consumer.pop(t)) {
                if (t.timestamp_us != static_cast<int64_t>(received)) {
                    order_ok = false;
                }
                if (t.price != static_cast<double>(received) * 10.0) {
                    order_ok = false;
                }
                received++;
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }

        // exit code: 0 = all OK, 1 = count mismatch, 2 = order error
        if (received != num_items) _exit(1);
        if (!order_ok) _exit(2);
        _exit(0);
    }

    // === 부모 프로세스: Producer ===
    // 약간의 지연 후 데이터 전송
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    for (size_t i = 0; i < num_items; ++i) {
        Ticker t{};
        t.exchange = Exchange::Binance;
        t.set_symbol("XRPUSDT");
        t.price = static_cast<double>(i) * 10.0;
        t.timestamp_us = static_cast<int64_t>(i);

        while (!producer.push(t)) {
            // busy wait if full (shouldn't happen with capacity=256)
        }
    }

    // 자식 프로세스 대기
    int status = 0;
    waitpid(child, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        TEST("Fork: child received all items", code == 0);
        if (code == 1) printf("  Child: count mismatch\n");
        if (code == 2) printf("  Child: order error\n");
    } else {
        TEST("Fork: child exited normally", false);
    }

    // is_producer_alive (부모에서 체크 — 자식은 이미 종료)
    TEST("is_producer_alive (self)", producer.is_producer_alive());
}

// =============================================================================
// Test 11: 성능 벤치마크
// =============================================================================
void test_benchmark() {
    printf("\n=== Performance Benchmark ===\n");

    const char* name = "/kimchi_test_bench";
    const size_t capacity = 4096;
    const size_t shm_size = shm_queue_size(capacity, sizeof(Ticker));
    const size_t iterations = 1000000;

    ShmSegment::unlink(name);
    ShmSegment seg(name, shm_size, true);

    auto producer = ShmSPSCQueue<Ticker>::init_producer(seg.data(), capacity);
    auto consumer = ShmSPSCQueue<Ticker>::attach_consumer(seg.data());

    Ticker t{};
    t.exchange = Exchange::Upbit;
    t.set_symbol("KRW-XRP");
    t.price = 3100.0;

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        while (!producer.push(t)) {
            Ticker out{};
            consumer.pop(out);
        }
    }
    // Drain
    Ticker out{};
    while (consumer.pop(out)) {}

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double ns_per_op = static_cast<double>(elapsed_ns) / static_cast<double>(iterations);
    double ops_per_sec = 1e9 / ns_per_op;

    printf("  Push+Pop: %.1f ns/op (%.0f ops/sec)\n", ns_per_op, ops_per_sec);
    TEST("Benchmark: < 100 ns/op", ns_per_op < 100.0);
}

// =============================================================================
// Main
// =============================================================================
int main() {
    printf("=== TASK_36: ShmSPSCQueue Test ===\n");

    test_static_assertions();
    test_shm_names();
    test_queue_size();
    test_shm_segment();
    test_single_process();
    test_queue_full();
    test_close();
    test_bulk_ordering();
    test_invalid_attach();
    test_fork();
    test_benchmark();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed (total %d)\n",
           tests_passed, tests_failed, tests_passed + tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
