/**
 * OrderTracker + UpbitPrivateWebSocket 테스트
 *
 * 1. client_order_id 파싱 테스트
 * 2. 정상 순서: register → WS update → completion
 * 3. 역전 순서: WS update → register → completion
 * 4. 부분 체결 → 완전 체결 시나리오
 * 5. 매수/매도 쌍 완료 콜백
 * 6. stale cleanup
 */

#include "arbitrage/executor/order_tracker.hpp"
#include "arbitrage/common/types.hpp"

#include <iostream>
#include <cstring>
#include <cassert>
#include <atomic>
#include <thread>
#include <chrono>

using namespace arbitrage;

// =============================================================================
// 테스트 유틸
// =============================================================================

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    std::cout << "  [TEST] " << name << "... "; \
    try

#define PASS() \
    std::cout << "PASS\n"; \
    tests_passed++;

#define FAIL(msg) \
    std::cout << "FAIL: " << msg << "\n"; \
    tests_failed++;

#define ASSERT_TRUE(cond) \
    if (!(cond)) { FAIL(#cond); return; }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { FAIL(#a " != " #b); return; }

// =============================================================================
// 1. client_order_id 파싱 테스트
// =============================================================================

void test_parse_client_order_id() {
    std::cout << "\n=== client_order_id 파싱 ===\n";

    TEST("정상 buy 파싱") {
        int64_t request_id;
        bool is_buy;
        bool ok = OrderTracker::parse_client_order_id("arb_1660801715431_buy",
                                                       request_id, is_buy);
        ASSERT_TRUE(ok);
        ASSERT_EQ(request_id, 1660801715431LL);
        ASSERT_TRUE(is_buy);
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("정상 sell 파싱") {
        int64_t request_id;
        bool is_buy;
        bool ok = OrderTracker::parse_client_order_id("arb_9999999_sell",
                                                       request_id, is_buy);
        ASSERT_TRUE(ok);
        ASSERT_EQ(request_id, 9999999LL);
        ASSERT_TRUE(!is_buy);
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("잘못된 접두사") {
        int64_t request_id;
        bool is_buy;
        bool ok = OrderTracker::parse_client_order_id("xxx_123_buy",
                                                       request_id, is_buy);
        ASSERT_TRUE(!ok);
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("잘못된 side") {
        int64_t request_id;
        bool is_buy;
        bool ok = OrderTracker::parse_client_order_id("arb_123_hold",
                                                       request_id, is_buy);
        ASSERT_TRUE(!ok);
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("빈 문자열") {
        int64_t request_id;
        bool is_buy;
        bool ok = OrderTracker::parse_client_order_id("", request_id, is_buy);
        ASSERT_TRUE(!ok);
        PASS();
    } catch (...) { FAIL("exception"); }
}

// =============================================================================
// 2. 정상 순서: register → WS update → completion
// =============================================================================

void test_normal_flow() {
    std::cout << "\n=== 정상 순서 (register → WS) ===\n";

    TEST("매수/매도 양쪽 체결 → completion 콜백") {
        std::atomic<int> complete_count{0};
        int64_t completed_request_id = 0;

        OrderTracker tracker([&](int64_t req_id,
                                  const TrackedOrder& buy,
                                  const TrackedOrder& sell) {
            complete_count++;
            completed_request_id = req_id;
        });

        // register: 매수/매도 등록
        tracker.register_order("arb_100_buy", "UPBIT-UUID-1");
        tracker.register_order("arb_100_sell", "BINANCE-12345");

        ASSERT_EQ(tracker.active_count(), static_cast<size_t>(2));
        ASSERT_EQ(complete_count.load(), 0);

        // WS update: 매수 체결
        OrderUpdate buy_update{};
        buy_update.exchange = Exchange::Binance;
        buy_update.set_client_order_id("arb_100_buy");
        buy_update.set_order_id("UPBIT-UUID-1");
        buy_update.status = OrderStatus::Filled;
        buy_update.filled_qty = 100.0;
        buy_update.avg_price = 0.65;
        tracker.on_order_update(buy_update);

        // 아직 매도 미체결 → completion 안 됨
        ASSERT_EQ(complete_count.load(), 0);

        // WS update: 매도 체결
        OrderUpdate sell_update{};
        sell_update.exchange = Exchange::Upbit;
        sell_update.set_client_order_id("arb_100_sell");
        sell_update.set_order_id("BINANCE-12345");
        sell_update.status = OrderStatus::Filled;
        sell_update.filled_qty = 100.0;
        sell_update.avg_price = 700.0;
        tracker.on_order_update(sell_update);

        // 양쪽 체결 → completion 호출
        ASSERT_EQ(complete_count.load(), 1);
        ASSERT_EQ(completed_request_id, 100LL);

        PASS();
    } catch (...) { FAIL("exception"); }
}

// =============================================================================
// 3. 역전 순서: WS update → register
// =============================================================================

void test_reverse_order() {
    std::cout << "\n=== 역전 순서 (WS → register) ===\n";

    TEST("WS가 REST보다 먼저 도착") {
        std::atomic<int> complete_count{0};

        OrderTracker tracker([&](int64_t req_id,
                                  const TrackedOrder& buy,
                                  const TrackedOrder& sell) {
            complete_count++;
        });

        // WS가 먼저 도착 (REST 응답 전)
        OrderUpdate buy_update{};
        buy_update.exchange = Exchange::Binance;
        buy_update.set_client_order_id("arb_200_buy");
        buy_update.set_order_id("BN-99999");
        buy_update.status = OrderStatus::Filled;
        buy_update.filled_qty = 50.0;
        tracker.on_order_update(buy_update);

        // 추적 중이어야 함 (WS가 자동으로 슬롯 생성)
        ASSERT_EQ(tracker.active_count(), static_cast<size_t>(1));

        // REST 응답이 나중에 도착 → register
        tracker.register_order("arb_200_buy", "BN-99999");

        // 매도도 처리
        OrderUpdate sell_update{};
        sell_update.exchange = Exchange::Upbit;
        sell_update.set_client_order_id("arb_200_sell");
        sell_update.status = OrderStatus::Filled;
        sell_update.filled_qty = 50.0;
        tracker.on_order_update(sell_update);

        tracker.register_order("arb_200_sell", "UP-88888");

        ASSERT_EQ(complete_count.load(), 1);

        PASS();
    } catch (...) { FAIL("exception"); }
}

// =============================================================================
// 4. 부분 체결 → 완전 체결
// =============================================================================

void test_partial_fill() {
    std::cout << "\n=== 부분 체결 → 완전 체결 ===\n";

    TEST("PartiallyFilled → Filled") {
        std::atomic<int> complete_count{0};

        OrderTracker tracker([&](int64_t req_id,
                                  const TrackedOrder& buy,
                                  const TrackedOrder& sell) {
            complete_count++;
        });

        tracker.register_order("arb_300_buy");
        tracker.register_order("arb_300_sell");

        // 매수: 부분 체결 (terminal 아님 → completion 안 됨)
        OrderUpdate partial{};
        partial.set_client_order_id("arb_300_buy");
        partial.status = OrderStatus::PartiallyFilled;
        partial.filled_qty = 30.0;
        partial.remaining_qty = 70.0;
        tracker.on_order_update(partial);

        ASSERT_EQ(complete_count.load(), 0);

        // 매수: 완전 체결
        OrderUpdate filled{};
        filled.set_client_order_id("arb_300_buy");
        filled.status = OrderStatus::Filled;
        filled.filled_qty = 100.0;
        filled.remaining_qty = 0.0;
        tracker.on_order_update(filled);

        // 아직 매도 미처리
        ASSERT_EQ(complete_count.load(), 0);

        // 매도: 완전 체결
        OrderUpdate sell_filled{};
        sell_filled.set_client_order_id("arb_300_sell");
        sell_filled.status = OrderStatus::Filled;
        sell_filled.filled_qty = 100.0;
        tracker.on_order_update(sell_filled);

        ASSERT_EQ(complete_count.load(), 1);

        PASS();
    } catch (...) { FAIL("exception"); }
}

// =============================================================================
// 5. 한쪽 실패 (Cancel)
// =============================================================================

void test_one_side_cancel() {
    std::cout << "\n=== 한쪽 취소 ===\n";

    TEST("매수 체결 + 매도 취소 → completion 호출 (recovery 필요)") {
        std::atomic<int> complete_count{0};
        OrderStatus buy_final{};
        OrderStatus sell_final{};

        OrderTracker tracker([&](int64_t req_id,
                                  const TrackedOrder& buy,
                                  const TrackedOrder& sell) {
            complete_count++;
            buy_final = buy.last_update.status;
            sell_final = sell.last_update.status;
        });

        tracker.register_order("arb_400_buy");
        tracker.register_order("arb_400_sell");

        // 매수 체결
        OrderUpdate buy_ok{};
        buy_ok.set_client_order_id("arb_400_buy");
        buy_ok.status = OrderStatus::Filled;
        buy_ok.filled_qty = 100.0;
        tracker.on_order_update(buy_ok);

        // 매도 취소
        OrderUpdate sell_cancel{};
        sell_cancel.set_client_order_id("arb_400_sell");
        sell_cancel.status = OrderStatus::Canceled;
        tracker.on_order_update(sell_cancel);

        // 양쪽 terminal → completion
        ASSERT_EQ(complete_count.load(), 1);
        ASSERT_EQ(buy_final, OrderStatus::Filled);
        ASSERT_EQ(sell_final, OrderStatus::Canceled);

        PASS();
    } catch (...) { FAIL("exception"); }
}

// =============================================================================
// 6. Stale cleanup
// =============================================================================

void test_cleanup() {
    std::cout << "\n=== Stale cleanup ===\n";

    TEST("completed 엔트리 정리") {
        OrderTracker tracker([](int64_t, const TrackedOrder&, const TrackedOrder&) {});

        tracker.register_order("arb_500_buy");
        tracker.register_order("arb_500_sell");

        OrderUpdate buy_done{};
        buy_done.set_client_order_id("arb_500_buy");
        buy_done.status = OrderStatus::Filled;
        tracker.on_order_update(buy_done);

        OrderUpdate sell_done{};
        sell_done.set_client_order_id("arb_500_sell");
        sell_done.status = OrderStatus::Filled;
        tracker.on_order_update(sell_done);

        ASSERT_EQ(tracker.active_count(), static_cast<size_t>(2));

        // cleanup → completed 엔트리 제거
        tracker.cleanup_stale();

        ASSERT_EQ(tracker.active_count(), static_cast<size_t>(0));

        PASS();
    } catch (...) { FAIL("exception"); }
}

// =============================================================================
// 7. OrderUpdate 구조체 기본 테스트
// =============================================================================

void test_order_update_struct() {
    std::cout << "\n=== OrderUpdate 구조체 ===\n";

    TEST("trivially_copyable 확인") {
        ASSERT_TRUE(std::is_trivially_copyable_v<OrderUpdate>);
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("set/get 메서드") {
        OrderUpdate u{};
        u.set_order_id("UUID-12345");
        u.set_client_order_id("arb_999_buy");
        u.status = OrderStatus::Filled;

        ASSERT_TRUE(std::strcmp(u.order_id, "UUID-12345") == 0);
        ASSERT_TRUE(std::strcmp(u.client_order_id, "arb_999_buy") == 0);
        ASSERT_TRUE(u.is_terminal());
        ASSERT_TRUE(u.has_client_order_id());
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("is_terminal 판정") {
        OrderUpdate u{};
        u.status = OrderStatus::Open;
        ASSERT_TRUE(!u.is_terminal());

        u.status = OrderStatus::PartiallyFilled;
        ASSERT_TRUE(!u.is_terminal());

        u.status = OrderStatus::Filled;
        ASSERT_TRUE(u.is_terminal());

        u.status = OrderStatus::Canceled;
        ASSERT_TRUE(u.is_terminal());

        u.status = OrderStatus::Failed;
        ASSERT_TRUE(u.is_terminal());
        PASS();
    } catch (...) { FAIL("exception"); }

    TEST("string_view set 메서드") {
        OrderUpdate u{};
        std::string_view sv = "test-order-id-123";
        u.set_order_id(sv);
        ASSERT_TRUE(std::strcmp(u.order_id, "test-order-id-123") == 0);
        PASS();
    } catch (...) { FAIL("exception"); }
}

// =============================================================================
// 8. 다중 동시 주문
// =============================================================================

void test_multiple_pairs() {
    std::cout << "\n=== 다중 주문 쌍 ===\n";

    TEST("3개 쌍 동시 추적") {
        std::atomic<int> complete_count{0};

        OrderTracker tracker([&](int64_t req_id,
                                  const TrackedOrder& buy,
                                  const TrackedOrder& sell) {
            complete_count++;
        });

        // 3개 쌍 등록
        for (int i = 1; i <= 3; ++i) {
            char buy_id[48], sell_id[48];
            snprintf(buy_id, sizeof(buy_id), "arb_%d_buy", i * 100);
            snprintf(sell_id, sizeof(sell_id), "arb_%d_sell", i * 100);
            tracker.register_order(buy_id);
            tracker.register_order(sell_id);
        }

        ASSERT_EQ(tracker.active_count(), static_cast<size_t>(6));

        // 쌍 2만 완료
        OrderUpdate u{};
        u.set_client_order_id("arb_200_buy");
        u.status = OrderStatus::Filled;
        tracker.on_order_update(u);

        u.set_client_order_id("arb_200_sell");
        u.status = OrderStatus::Filled;
        tracker.on_order_update(u);

        ASSERT_EQ(complete_count.load(), 1);

        // 쌍 1, 3도 완료
        u.set_client_order_id("arb_100_buy");
        u.status = OrderStatus::Filled;
        tracker.on_order_update(u);

        u.set_client_order_id("arb_100_sell");
        u.status = OrderStatus::Canceled;
        tracker.on_order_update(u);

        u.set_client_order_id("arb_300_buy");
        u.status = OrderStatus::Filled;
        tracker.on_order_update(u);

        u.set_client_order_id("arb_300_sell");
        u.status = OrderStatus::Filled;
        tracker.on_order_update(u);

        ASSERT_EQ(complete_count.load(), 3);

        PASS();
    } catch (...) { FAIL("exception"); }
}

// =============================================================================
// main
// =============================================================================

int main() {
    std::cout << "========================================\n";
    std::cout << " OrderTracker + OrderUpdate 테스트\n";
    std::cout << "========================================\n";

    test_order_update_struct();
    test_parse_client_order_id();
    test_normal_flow();
    test_reverse_order();
    test_partial_fill();
    test_one_side_cancel();
    test_cleanup();
    test_multiple_pairs();

    std::cout << "\n========================================\n";
    std::cout << " 결과: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "========================================\n";

    return tests_failed > 0 ? 1 : 0;
}
