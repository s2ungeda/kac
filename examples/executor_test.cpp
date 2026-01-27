/**
 * TASK_08: Executor Test
 *
 * 동시 주문 실행기 테스트 프로그램
 * - DualOrderExecutor 기능 검증
 * - RecoveryManager 복구 로직 검증
 * - std::async 병렬 실행 테스트
 */

#include "arbitrage/executor/dual_order.hpp"
#include "arbitrage/executor/recovery.hpp"
#include "arbitrage/exchange/order_base.hpp"

#include <iostream>
#include <chrono>
#include <thread>
#include <iomanip>
#include <cstring>

using namespace arbitrage;

// =============================================================================
// Mock Order Client (테스트용)
// =============================================================================
class MockOrderClient : public OrderClientBase {
public:
    MockOrderClient(Exchange ex, bool should_succeed = true,
                    Duration latency = Duration(10000))  // 10ms default
        : exchange_(ex)
        , should_succeed_(should_succeed)
        , latency_(latency)
    {}

    Result<OrderResult> place_order(const OrderRequest& req) override {
        // 지연 시뮬레이션
        std::this_thread::sleep_for(latency_);

        ++order_count_;

        if (!should_succeed_ || (fail_count_ > 0 && order_count_ <= fail_count_)) {
            return Err<OrderResult>(ErrorCode::ExchangeError,
                                   "Mock order failed (simulated)");
        }

        OrderResult result;
        char order_id[64];
        snprintf(order_id, sizeof(order_id), "MOCK-%s-%d",
                 exchange_name(exchange_), order_count_.load());
        result.set_order_id(order_id);
        result.status = OrderStatus::Filled;
        result.filled_qty = req.quantity;
        result.avg_price = req.price > 0 ? req.price : get_mock_price(req);
        result.commission = result.filled_qty * result.avg_price * 0.001;  // 0.1%
        result.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        return Ok(std::move(result));
    }

    Result<OrderResult> cancel_order(const std::string& order_id) override {
        std::this_thread::sleep_for(latency_);

        OrderResult result;
        result.set_order_id(order_id.c_str());
        result.status = OrderStatus::Canceled;
        return Ok(std::move(result));
    }

    Result<OrderResult> get_order(const std::string& order_id) override {
        std::this_thread::sleep_for(latency_);

        OrderResult result;
        result.set_order_id(order_id.c_str());
        result.status = OrderStatus::Filled;
        return Ok(std::move(result));
    }

    Result<Balance> get_balance(const std::string& currency) override {
        std::this_thread::sleep_for(latency_);

        Balance balance;
        balance.set_currency(currency.c_str());
        balance.available = 10000.0;
        balance.locked = 0.0;
        return Ok(std::move(balance));
    }

    Exchange exchange() const override { return exchange_; }
    std::string name() const override { return exchange_name(exchange_); }

    // 테스트 설정
    void set_should_succeed(bool success) { should_succeed_ = success; }
    void set_latency(Duration lat) { latency_ = lat; }
    void set_fail_count(int count) { fail_count_ = count; }
    void reset_order_count() { order_count_ = 0; }
    int get_order_count() const { return order_count_; }

private:
    double get_mock_price(const OrderRequest& req) {
        // 거래소별 모의 가격
        if (is_krw_exchange(exchange_)) {
            return 3100.0;  // KRW
        } else {
            return 2.15;    // USDT
        }
    }

    Exchange exchange_;
    bool should_succeed_;
    Duration latency_;
    int fail_count_{0};
    std::atomic<int> order_count_{0};
};

// =============================================================================
// 테스트 함수들
// =============================================================================

void print_separator(const char* title) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << " " << title << "\n";
    std::cout << std::string(60, '=') << "\n";
}

void print_result(const DualOrderResult& result) {
    auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        result.total_latency()).count();

    std::cout << "\n--- Dual Order Result ---\n";
    std::cout << "Request ID: " << result.request_id << "\n";
    std::cout << "Total Latency: " << latency_ms << " ms\n";
    std::cout << "Both Success: " << (result.both_success() ? "YES" : "NO") << "\n";
    std::cout << "Partial Fill: " << (result.partial_fill() ? "YES" : "NO") << "\n";

    std::cout << "\nBuy Order:\n";
    std::cout << "  Exchange: " << exchange_name(result.buy_result.exchange) << "\n";
    std::cout << "  Success: " << (result.buy_result.is_success() ? "YES" : "NO") << "\n";
    if (result.buy_result.is_success()) {
        std::cout << "  Order ID: " << result.buy_result.order_id() << "\n";
        std::cout << "  Filled: " << std::fixed << std::setprecision(4)
                  << result.buy_result.filled_qty() << " @ "
                  << std::setprecision(2) << result.buy_result.avg_price() << "\n";
    } else {
        std::cout << "  Error: " << result.buy_result.error_message() << "\n";
    }
    std::cout << "  Latency: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     result.buy_result.latency).count() << " ms\n";

    std::cout << "\nSell Order:\n";
    std::cout << "  Exchange: " << exchange_name(result.sell_result.exchange) << "\n";
    std::cout << "  Success: " << (result.sell_result.is_success() ? "YES" : "NO") << "\n";
    if (result.sell_result.is_success()) {
        std::cout << "  Order ID: " << result.sell_result.order_id() << "\n";
        std::cout << "  Filled: " << std::fixed << std::setprecision(4)
                  << result.sell_result.filled_qty() << " @ "
                  << std::setprecision(2) << result.sell_result.avg_price() << "\n";
    } else {
        std::cout << "  Error: " << result.sell_result.error_message() << "\n";
    }
    std::cout << "  Latency: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     result.sell_result.latency).count() << " ms\n";
}

void print_stats(const ExecutorStats& stats) {
    std::cout << "\n--- Executor Stats ---\n";
    std::cout << "Total Requests: " << stats.total_requests.load() << "\n";
    std::cout << "Successful Dual: " << stats.successful_dual.load() << "\n";
    std::cout << "Partial Success: " << stats.partial_success.load() << "\n";
    std::cout << "Total Failures: " << stats.total_failures.load() << "\n";
    std::cout << "Success Rate: " << std::fixed << std::setprecision(1)
              << stats.success_rate() << "%\n";
    std::cout << "Avg Latency: " << std::setprecision(2)
              << stats.avg_latency_us() / 1000.0 << " ms\n";
    std::cout << "Recovery Attempts: " << stats.recovery_attempts.load() << "\n";
    std::cout << "Recovery Success: " << stats.recovery_success.load() << "\n";
    std::cout << "Recovery Rate: " << std::setprecision(1)
              << stats.recovery_rate() << "%\n";
}

// 테스트 1: 기본 동시 실행 (양쪽 성공)
bool test_basic_dual_execution() {
    print_separator("Test 1: Basic Dual Execution (Both Success)");

    // Mock 클라이언트 생성
    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance, true, Duration(15000));
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit, true, Duration(20000));

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    // Executor 생성
    DualOrderExecutor executor(clients);

    // 요청 생성
    DualOrderRequest request;
    request.set_request_id_auto();
    request.expected_premium = 2.5;

    // 매수 주문 (해외: Binance)
    request.buy_order.exchange = Exchange::Binance;
    request.buy_order.set_symbol("XRPUSDT");
    request.buy_order.side = OrderSide::Buy;
    request.buy_order.type = OrderType::Market;
    request.buy_order.quantity = 100.0;
    request.buy_order.price = 2.15;

    // 매도 주문 (국내: Upbit)
    request.sell_order.exchange = Exchange::Upbit;
    request.sell_order.set_symbol("KRW-XRP");
    request.sell_order.side = OrderSide::Sell;
    request.sell_order.type = OrderType::Market;
    request.sell_order.quantity = 100.0;
    request.sell_order.price = 3100.0;

    std::cout << "Executing dual order...\n";
    auto result = executor.execute_sync(request);

    print_result(result);
    print_stats(executor.stats());

    bool passed = result.both_success();
    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 2: 병렬 실행 검증 (지연 시간 비교)
bool test_parallel_execution() {
    print_separator("Test 2: Parallel Execution Verification");

    // 각 거래소에 50ms 지연 설정
    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance, true, Duration(50000));
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit, true, Duration(50000));

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    DualOrderExecutor executor(clients);

    DualOrderRequest request;
    request.set_request_id_auto();
    request.buy_order.exchange = Exchange::Binance;
    request.buy_order.set_symbol("XRPUSDT");
    request.buy_order.side = OrderSide::Buy;
    request.buy_order.type = OrderType::Market;
    request.buy_order.quantity = 100.0;

    request.sell_order.exchange = Exchange::Upbit;
    request.sell_order.set_symbol("KRW-XRP");
    request.sell_order.side = OrderSide::Sell;
    request.sell_order.type = OrderType::Market;
    request.sell_order.quantity = 100.0;

    auto start = std::chrono::steady_clock::now();
    auto result = executor.execute_sync(request);
    auto end = std::chrono::steady_clock::now();

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();

    std::cout << "Each order latency: 50ms\n";
    std::cout << "Total execution time: " << total_ms << "ms\n";
    std::cout << "Expected if sequential: ~100ms\n";
    std::cout << "Expected if parallel: ~50ms\n";

    // 병렬 실행이면 총 시간이 80ms 미만이어야 함 (마진 포함)
    bool passed = total_ms < 80 && result.both_success();
    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED");
    std::cout << " (parallel execution " << (total_ms < 80 ? "confirmed" : "failed") << ")\n";
    return passed;
}

// 테스트 3: 부분 체결 감지 (매수 성공, 매도 실패)
bool test_partial_fill_detection() {
    print_separator("Test 3: Partial Fill Detection (Buy OK, Sell FAIL)");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance, true, Duration(10000));
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit, false, Duration(10000));  // 실패

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    DualOrderExecutor executor(clients);
    executor.set_auto_recovery(false);  // 복구 비활성화

    DualOrderRequest request;
    request.set_request_id_auto();
    request.buy_order.exchange = Exchange::Binance;
    request.buy_order.set_symbol("XRPUSDT");
    request.buy_order.side = OrderSide::Buy;
    request.buy_order.type = OrderType::Market;
    request.buy_order.quantity = 100.0;

    request.sell_order.exchange = Exchange::Upbit;
    request.sell_order.set_symbol("KRW-XRP");
    request.sell_order.side = OrderSide::Sell;
    request.sell_order.type = OrderType::Market;
    request.sell_order.quantity = 100.0;

    auto result = executor.execute_sync(request);

    print_result(result);

    bool passed = result.partial_fill() &&
                  result.buy_result.is_success() &&
                  result.sell_result.is_failed();

    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 4: 복구 계획 생성
bool test_recovery_plan_creation() {
    print_separator("Test 4: Recovery Plan Creation");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance, true, Duration(10000));
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit, true, Duration(10000));

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    RecoveryManager recovery(clients);

    // 시나리오 1: 매수 성공, 매도 실패 → SellBought
    {
        std::cout << "\nScenario 1: Buy OK, Sell FAIL -> SellBought\n";

        DualOrderRequest request;
        request.buy_order.exchange = Exchange::Binance;
        request.buy_order.set_symbol("XRPUSDT");
        request.buy_order.side = OrderSide::Buy;
        request.buy_order.quantity = 100.0;

        request.sell_order.exchange = Exchange::Upbit;
        request.sell_order.set_symbol("KRW-XRP");
        request.sell_order.side = OrderSide::Sell;
        request.sell_order.quantity = 100.0;

        DualOrderResult result;
        result.buy_result.exchange = Exchange::Binance;
        result.buy_result.result = OrderResult{};
        result.buy_result.result->status = OrderStatus::Filled;
        result.buy_result.result->filled_qty = 100.0;
        result.buy_result.result->avg_price = 2.15;

        result.sell_result.exchange = Exchange::Upbit;
        result.sell_result.error = Error{ErrorCode::ExchangeError, "Sell failed"};

        auto plan = recovery.create_plan(request, result);

        std::cout << "  Action: " << recovery_action_name(plan.action) << "\n";
        std::cout << "  Reason: " << plan.reason << "\n";
        std::cout << "  Recovery Order: "
                  << (plan.recovery_order.side == OrderSide::Sell ? "SELL" : "BUY")
                  << " " << plan.recovery_order.quantity
                  << " " << plan.recovery_order.symbol
                  << " on " << exchange_name(plan.recovery_order.exchange) << "\n";

        if (plan.action != RecoveryAction::SellBought) {
            std::cout << "  ERROR: Expected SellBought action\n";
            return false;
        }
    }

    // 시나리오 2: 매수 실패, 매도 성공 → BuySold
    {
        std::cout << "\nScenario 2: Buy FAIL, Sell OK -> BuySold\n";

        DualOrderRequest request;
        request.buy_order.exchange = Exchange::Binance;
        request.buy_order.set_symbol("XRPUSDT");
        request.buy_order.side = OrderSide::Buy;
        request.buy_order.quantity = 100.0;

        request.sell_order.exchange = Exchange::Upbit;
        request.sell_order.set_symbol("KRW-XRP");
        request.sell_order.side = OrderSide::Sell;
        request.sell_order.quantity = 100.0;

        DualOrderResult result;
        result.buy_result.exchange = Exchange::Binance;
        result.buy_result.error = Error{ErrorCode::ExchangeError, "Buy failed"};

        result.sell_result.exchange = Exchange::Upbit;
        result.sell_result.result = OrderResult{};
        result.sell_result.result->status = OrderStatus::Filled;
        result.sell_result.result->filled_qty = 100.0;
        result.sell_result.result->avg_price = 3100.0;

        auto plan = recovery.create_plan(request, result);

        std::cout << "  Action: " << recovery_action_name(plan.action) << "\n";
        std::cout << "  Reason: " << plan.reason << "\n";
        std::cout << "  Recovery Order: "
                  << (plan.recovery_order.side == OrderSide::Sell ? "SELL" : "BUY")
                  << " " << plan.recovery_order.quantity
                  << " " << plan.recovery_order.symbol
                  << " on " << exchange_name(plan.recovery_order.exchange) << "\n";

        if (plan.action != RecoveryAction::BuySold) {
            std::cout << "  ERROR: Expected BuySold action\n";
            return false;
        }
    }

    // 시나리오 3: 둘 다 성공 → None
    {
        std::cout << "\nScenario 3: Both OK -> None\n";

        DualOrderRequest request;
        request.buy_order.exchange = Exchange::Binance;
        request.sell_order.exchange = Exchange::Upbit;

        DualOrderResult result;
        result.buy_result.exchange = Exchange::Binance;
        result.buy_result.result = OrderResult{};
        result.buy_result.result->status = OrderStatus::Filled;

        result.sell_result.exchange = Exchange::Upbit;
        result.sell_result.result = OrderResult{};
        result.sell_result.result->status = OrderStatus::Filled;

        auto plan = recovery.create_plan(request, result);

        std::cout << "  Action: " << recovery_action_name(plan.action) << "\n";

        if (plan.action != RecoveryAction::None) {
            std::cout << "  ERROR: Expected None action\n";
            return false;
        }
    }

    std::cout << "\nTest Result: PASSED\n";
    return true;
}

// 테스트 5: 복구 실행 (Dry Run)
bool test_recovery_execution_dry_run() {
    print_separator("Test 5: Recovery Execution (Dry Run)");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance, true, Duration(10000));
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit, true, Duration(10000));

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    RecoveryManager recovery(clients);
    recovery.set_dry_run(true);  // 실제 주문 없이 테스트

    // 복구 계획 생성
    RecoveryPlan plan;
    plan.action = RecoveryAction::SellBought;
    plan.reason = "Test recovery";
    plan.recovery_order.exchange = Exchange::Binance;
    plan.recovery_order.set_symbol("XRPUSDT");
    plan.recovery_order.side = OrderSide::Sell;
    plan.recovery_order.type = OrderType::Market;
    plan.recovery_order.quantity = 100.0;
    plan.max_retries = 3;

    std::cout << "Executing recovery (dry run)...\n";
    auto result = recovery.execute_recovery(plan);

    std::cout << "\nRecovery Result:\n";
    std::cout << "  Success: " << (result.success ? "YES" : "NO") << "\n";
    std::cout << "  Message: " << result.message << "\n";
    std::cout << "  Retries: " << result.plan.retry_count << "\n";

    const auto& stats = recovery.stats();
    std::cout << "\nRecovery Stats:\n";
    std::cout << "  Total Plans: " << stats.total_plans.load() << "\n";
    std::cout << "  Executions: " << stats.executions.load() << "\n";
    std::cout << "  Successes: " << stats.successes.load() << "\n";

    bool passed = result.success;
    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 6: 실제 복구 실행
bool test_recovery_execution_real() {
    print_separator("Test 6: Recovery Execution (Real)");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance, true, Duration(10000));
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit, true, Duration(10000));

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    RecoveryManager recovery(clients);
    recovery.set_dry_run(false);  // 실제 실행

    // 복구 계획 생성
    RecoveryPlan plan;
    plan.action = RecoveryAction::SellBought;
    plan.reason = "Test recovery";
    plan.recovery_order.exchange = Exchange::Binance;
    plan.recovery_order.set_symbol("XRPUSDT");
    plan.recovery_order.side = OrderSide::Sell;
    plan.recovery_order.type = OrderType::Market;
    plan.recovery_order.quantity = 100.0;
    plan.max_retries = 3;

    std::cout << "Executing recovery (real)...\n";
    auto result = recovery.execute_recovery(plan);

    std::cout << "\nRecovery Result:\n";
    std::cout << "  Success: " << (result.success ? "YES" : "NO") << "\n";
    std::cout << "  Message: " << result.message << "\n";
    if (result.result.is_success()) {
        std::cout << "  Order ID: " << result.result.order_id() << "\n";
        std::cout << "  Filled: " << result.result.filled_qty()
                  << " @ " << result.result.avg_price() << "\n";
    }

    bool passed = result.success && result.result.is_filled();
    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 7: 복구 재시도 로직
bool test_recovery_retry() {
    print_separator("Test 7: Recovery Retry Logic");

    // 처음 2번 실패 후 성공하도록 설정
    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance, true, Duration(5000));
    binance->set_fail_count(2);  // 처음 2번 실패

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;

    RecoveryManager recovery(clients);
    recovery.set_max_retries(5);
    recovery.set_retry_delay(Duration(10000));  // 10ms

    RecoveryPlan plan;
    plan.action = RecoveryAction::SellBought;
    plan.reason = "Test retry";
    plan.recovery_order.exchange = Exchange::Binance;
    plan.recovery_order.set_symbol("XRPUSDT");
    plan.recovery_order.side = OrderSide::Sell;
    plan.recovery_order.type = OrderType::Market;
    plan.recovery_order.quantity = 100.0;
    plan.max_retries = 5;
    plan.retry_delay = Duration(10000);

    std::cout << "Executing recovery (should fail 2 times, then succeed)...\n";
    auto result = recovery.execute_recovery(plan);

    std::cout << "\nRecovery Result:\n";
    std::cout << "  Success: " << (result.success ? "YES" : "NO") << "\n";
    std::cout << "  Retries: " << result.plan.retry_count << "\n";
    std::cout << "  Order Count: " << binance->get_order_count() << "\n";

    const auto& stats = recovery.stats();
    std::cout << "\nRecovery Stats:\n";
    std::cout << "  Retries: " << stats.retries.load() << "\n";
    std::cout << "  Successes: " << stats.successes.load() << "\n";

    // 성공하고, 재시도 2번 했어야 함
    bool passed = result.success && result.plan.retry_count == 2;
    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 8: 자동 복구 통합 테스트
bool test_auto_recovery_integration() {
    print_separator("Test 8: Auto Recovery Integration");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance, true, Duration(10000));
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit, false, Duration(10000));  // 매도 실패

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    auto recovery = std::make_shared<RecoveryManager>(clients);
    recovery->set_dry_run(false);

    DualOrderExecutor executor(clients, recovery);
    executor.set_auto_recovery(true);

    bool recovery_called = false;
    executor.set_recovery_callback([&](const RecoveryResult& result) {
        recovery_called = true;
        std::cout << "\n[Recovery Callback]\n";
        std::cout << "  Action: " << recovery_action_name(result.plan.action) << "\n";
        std::cout << "  Success: " << (result.success ? "YES" : "NO") << "\n";
    });

    DualOrderRequest request;
    request.set_request_id_auto();
    request.buy_order.exchange = Exchange::Binance;
    request.buy_order.set_symbol("XRPUSDT");
    request.buy_order.side = OrderSide::Buy;
    request.buy_order.type = OrderType::Market;
    request.buy_order.quantity = 100.0;

    request.sell_order.exchange = Exchange::Upbit;
    request.sell_order.set_symbol("KRW-XRP");
    request.sell_order.side = OrderSide::Sell;
    request.sell_order.type = OrderType::Market;
    request.sell_order.quantity = 100.0;

    std::cout << "Executing dual order (sell will fail, auto recovery enabled)...\n";
    auto result = executor.execute_sync(request);

    print_result(result);
    print_stats(executor.stats());

    // 매도 실패 + 자동 복구 실행 확인
    bool passed = result.partial_fill() && recovery_called;
    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 9: 비동기 실행
bool test_async_execution() {
    print_separator("Test 9: Async Execution");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance, true, Duration(30000));
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit, true, Duration(30000));

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    DualOrderExecutor executor(clients);

    DualOrderRequest request;
    request.set_request_id_auto();
    request.buy_order.exchange = Exchange::Binance;
    request.buy_order.set_symbol("XRPUSDT");
    request.buy_order.side = OrderSide::Buy;
    request.buy_order.type = OrderType::Market;
    request.buy_order.quantity = 100.0;

    request.sell_order.exchange = Exchange::Upbit;
    request.sell_order.set_symbol("KRW-XRP");
    request.sell_order.side = OrderSide::Sell;
    request.sell_order.type = OrderType::Market;
    request.sell_order.quantity = 100.0;

    std::cout << "Launching async execution...\n";
    auto start = std::chrono::steady_clock::now();
    auto future = executor.execute_async(request);

    std::cout << "Async call returned immediately, doing other work...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "Waiting for result...\n";
    auto result = future.get();
    auto end = std::chrono::steady_clock::now();

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();

    std::cout << "Total time: " << total_ms << " ms\n";
    std::cout << "Both Success: " << (result.both_success() ? "YES" : "NO") << "\n";

    bool passed = result.both_success();
    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " TASK_08: Executor Test\n";
    std::cout << " Dual Order Executor & Recovery Manager\n";
    std::cout << "========================================\n";

    int passed = 0;
    int failed = 0;

    auto run_test = [&](const char* name, bool (*test)()) {
        try {
            if (test()) {
                ++passed;
            } else {
                ++failed;
            }
        } catch (const std::exception& e) {
            std::cerr << "Test " << name << " threw exception: " << e.what() << "\n";
            ++failed;
        }
    };

    run_test("Basic Dual Execution", test_basic_dual_execution);
    run_test("Parallel Execution", test_parallel_execution);
    run_test("Partial Fill Detection", test_partial_fill_detection);
    run_test("Recovery Plan Creation", test_recovery_plan_creation);
    run_test("Recovery Execution (Dry Run)", test_recovery_execution_dry_run);
    run_test("Recovery Execution (Real)", test_recovery_execution_real);
    run_test("Recovery Retry", test_recovery_retry);
    run_test("Auto Recovery Integration", test_auto_recovery_integration);
    run_test("Async Execution", test_async_execution);

    print_separator("Final Results");
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";
    std::cout << "Total:  " << (passed + failed) << "\n";

    if (failed == 0) {
        std::cout << "\n*** ALL TESTS PASSED ***\n";
    } else {
        std::cout << "\n*** SOME TESTS FAILED ***\n";
    }

    return failed == 0 ? 0 : 1;
}
