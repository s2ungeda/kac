/**
 * Event Bus Test (TASK_19)
 *
 * EventBus Pub/Sub 시스템 테스트
 * - 동기/비동기 이벤트 발행
 * - 타입 안전 구독
 * - 구독 해제
 * - RAII 구독 가드
 */

#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"

#include <iostream>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>

using namespace arbitrage;
using namespace arbitrage::events;

// 테스트 결과 카운터
static int tests_passed = 0;
static int tests_failed = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::cout << "[FAIL] " << msg << "\n";
        ++tests_failed;
    } else {
        std::cout << "[PASS] " << msg << "\n";
        ++tests_passed;
    }
}

// =============================================================================
// Test: Basic Publish/Subscribe (Sync)
// =============================================================================
void test_sync_publish_subscribe() {
    std::cout << "\n=== Test: sync_publish_subscribe ===\n";

    auto bus = std::make_shared<EventBus>();
    std::atomic<int> ticker_count{0};
    std::atomic<int> premium_count{0};

    // TickerReceived 구독
    auto token1 = bus->subscribe<TickerReceived>([&](const TickerReceived& e) {
        ++ticker_count;
    });

    // PremiumUpdated 구독
    auto token2 = bus->subscribe<PremiumUpdated>([&](const PremiumUpdated& e) {
        ++premium_count;
    });

    check(bus->subscriber_count() == 2, "Should have 2 subscribers");

    // 이벤트 발행
    bus->publish(TickerReceived());
    bus->publish(TickerReceived());
    bus->publish(PremiumUpdated());

    check(ticker_count == 2, "TickerReceived handler called twice");
    check(premium_count == 1, "PremiumUpdated handler called once");

    // 구독 해제
    bus->unsubscribe(token1);
    bus->publish(TickerReceived());

    check(ticker_count == 2, "After unsubscribe, handler not called");
    check(bus->subscriber_count() == 1, "Should have 1 subscriber after unsubscribe");
}

// =============================================================================
// Test: Async Publish/Subscribe
// =============================================================================
void test_async_publish_subscribe() {
    std::cout << "\n=== Test: async_publish_subscribe ===\n";

    auto bus = std::make_shared<EventBus>();
    std::atomic<int> event_count{0};

    bus->subscribe<OrderSubmitted>([&](const OrderSubmitted& e) {
        ++event_count;
    });

    // 비동기 모드 시작
    bus->start_async(2);
    check(bus->is_running(), "EventBus should be running");

    // 이벤트 발행
    for (int i = 0; i < 100; ++i) {
        bus->publish(OrderSubmitted());
    }

    // 처리 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 비동기 모드 중지
    bus->stop();
    check(!bus->is_running(), "EventBus should be stopped");

    check(event_count == 100, "All 100 events should be processed");
    check(bus->total_events_published() == 100, "Published 100 events");
    check(bus->total_events_dispatched() == 100, "Dispatched 100 events");
}

// =============================================================================
// Test: Subscribe All
// =============================================================================
void test_subscribe_all() {
    std::cout << "\n=== Test: subscribe_all ===\n";

    auto bus = std::make_shared<EventBus>();
    std::atomic<int> all_count{0};
    std::vector<std::string> event_types;
    std::mutex mtx;

    bus->subscribe_all([&](const Event& e) {
        ++all_count;
        std::lock_guard<std::mutex> lock(mtx);
        event_types.push_back(get_event_type_name(e));
    });

    bus->publish(TickerReceived());
    bus->publish(PremiumUpdated());
    bus->publish(OrderFilled());

    check(all_count == 3, "subscribe_all received all 3 events");
    check(event_types.size() == 3, "Captured 3 event types");
}

// =============================================================================
// Test: RAII Subscription Guard
// =============================================================================
void test_subscription_guard() {
    std::cout << "\n=== Test: subscription_guard ===\n";

    auto bus = std::make_shared<EventBus>();
    std::atomic<int> count{0};

    {
        auto guard = bus->subscribe_guarded<ExchangeConnected>([&](const ExchangeConnected& e) {
            ++count;
        });

        check(bus->subscriber_count() == 1, "Should have 1 subscriber");

        bus->publish(ExchangeConnected());
        check(count == 1, "Handler called once");
    }

    // 가드 소멸 후 자동 해제
    check(bus->subscriber_count() == 0, "Should have 0 subscribers after guard destroyed");

    bus->publish(ExchangeConnected());
    check(count == 1, "Handler not called after unsubscribe");
}

// =============================================================================
// Test: Type Safety
// =============================================================================
void test_type_safety() {
    std::cout << "\n=== Test: type_safety ===\n";

    auto bus = std::make_shared<EventBus>();
    std::atomic<int> order_count{0};
    std::atomic<int> transfer_count{0};

    bus->subscribe<OrderFilled>([&](const OrderFilled& e) {
        ++order_count;
    });

    bus->subscribe<TransferCompleted>([&](const TransferCompleted& e) {
        ++transfer_count;
    });

    // 다른 타입 이벤트 발행
    bus->publish(OrderFilled());
    bus->publish(TransferCompleted());
    bus->publish(KillSwitchActivated());

    check(order_count == 1, "OrderFilled handler called once");
    check(transfer_count == 1, "TransferCompleted handler called once");
}

// =============================================================================
// Test: Event Properties
// =============================================================================
void test_event_properties() {
    std::cout << "\n=== Test: event_properties ===\n";

    PremiumUpdated e;
    e.premium_pct = 5.5;
    e.buy_exchange = Exchange::Binance;
    e.sell_exchange = Exchange::Upbit;

    check(!e.id.empty(), "Event has ID");
    check(e.premium_pct == 5.5, "Premium percentage correct");
    check(e.buy_exchange == Exchange::Binance, "Buy exchange correct");
    check(e.sell_exchange == Exchange::Upbit, "Sell exchange correct");

    // Event 타입으로 변환
    Event event = e;
    check(get_event_type_name(event) == std::string("PremiumUpdated"), "Event type name correct");
    check(!get_event_id(event).empty(), "Event ID accessible via variant");
}

// =============================================================================
// Test: Concurrent Publishing
// =============================================================================
void test_concurrent_publishing() {
    std::cout << "\n=== Test: concurrent_publishing ===\n";

    auto bus = std::make_shared<EventBus>();
    std::atomic<int> count{0};

    bus->subscribe<TickerReceived>([&](const TickerReceived& e) {
        ++count;
    });

    bus->start_async(4);

    // 멀티스레드에서 이벤트 발행
    std::vector<std::thread> publishers;
    for (int t = 0; t < 4; ++t) {
        publishers.emplace_back([&bus]() {
            for (int i = 0; i < 250; ++i) {
                bus->publish(TickerReceived());
            }
        });
    }

    for (auto& t : publishers) {
        t.join();
    }

    // 처리 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    bus->stop();

    check(count == 1000, "All 1000 concurrent events processed");
}

// =============================================================================
// Test: Singleton Instance
// =============================================================================
void test_singleton() {
    std::cout << "\n=== Test: singleton ===\n";

    auto bus1 = EventBus::instance();
    auto bus2 = EventBus::instance();

    check(bus1 == bus2, "Singleton returns same instance");
}

// =============================================================================
// Test: Multiple Event Types
// =============================================================================
void test_multiple_event_types() {
    std::cout << "\n=== Test: multiple_event_types ===\n";

    auto bus = std::make_shared<EventBus>();
    std::atomic<int> system_events{0};
    std::atomic<int> order_events{0};

    // 시스템 이벤트들 구독
    bus->subscribe<ExchangeConnected>([&](const auto&) { ++system_events; });
    bus->subscribe<ExchangeDisconnected>([&](const auto&) { ++system_events; });
    bus->subscribe<KillSwitchActivated>([&](const auto&) { ++system_events; });

    // 주문 이벤트들 구독
    bus->subscribe<OrderSubmitted>([&](const auto&) { ++order_events; });
    bus->subscribe<OrderFilled>([&](const auto&) { ++order_events; });
    bus->subscribe<OrderCanceled>([&](const auto&) { ++order_events; });
    bus->subscribe<OrderFailed>([&](const auto&) { ++order_events; });

    check(bus->subscriber_count() == 7, "Should have 7 subscribers");

    bus->publish(ExchangeConnected(Exchange::Upbit));
    bus->publish(OrderSubmitted());
    bus->publish(OrderFilled());
    bus->publish(KillSwitchActivated("test"));

    check(system_events == 2, "2 system events received");
    check(order_events == 2, "2 order events received");
}

// =============================================================================
// Test: Handler Exception Safety
// =============================================================================
void test_handler_exception_safety() {
    std::cout << "\n=== Test: handler_exception_safety ===\n";

    auto bus = std::make_shared<EventBus>();
    std::atomic<int> count{0};

    // 예외를 던지는 핸들러
    bus->subscribe<TickerReceived>([](const TickerReceived&) {
        throw std::runtime_error("Handler error");
    });

    // 정상 핸들러
    bus->subscribe<TickerReceived>([&](const TickerReceived&) {
        ++count;
    });

    // 예외가 있어도 다른 핸들러는 실행되어야 함
    bus->publish(TickerReceived());

    check(count == 1, "Second handler called despite first handler exception");
}

// =============================================================================
// Main
// =============================================================================
int main() {
    std::cout << "========================================\n";
    std::cout << "  Event Bus Test (TASK_19)\n";
    std::cout << "========================================\n";

    test_sync_publish_subscribe();
    test_async_publish_subscribe();
    test_subscribe_all();
    test_subscription_guard();
    test_type_safety();
    test_event_properties();
    test_concurrent_publishing();
    test_singleton();
    test_multiple_event_types();
    test_handler_exception_safety();

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, "
              << tests_failed << " failed\n";
    std::cout << "========================================\n";

    return tests_failed > 0 ? 1 : 0;
}
