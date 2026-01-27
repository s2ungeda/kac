/**
 * TASK_09: Transfer Test
 *
 * 거래소 간 송금 관리자 테스트 프로그램
 * - TransferManager 기능 검증
 * - 출금/입금 흐름 테스트
 * - Destination Tag 처리 확인
 */

#include "arbitrage/executor/transfer.hpp"
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
    MockOrderClient(Exchange ex) : exchange_(ex) {}

    Result<OrderResult> place_order(const OrderRequest& req) override {
        OrderResult result;
        result.set_order_id("MOCK-ORDER-001");
        result.status = OrderStatus::Filled;
        result.filled_qty = req.quantity;
        result.avg_price = req.price > 0 ? req.price : 2.15;
        return Ok(std::move(result));
    }

    Result<OrderResult> cancel_order(const std::string& order_id) override {
        OrderResult result;
        result.set_order_id(order_id.c_str());
        result.status = OrderStatus::Canceled;
        return Ok(std::move(result));
    }

    Result<OrderResult> get_order(const std::string& order_id) override {
        OrderResult result;
        result.set_order_id(order_id.c_str());
        result.status = OrderStatus::Filled;
        return Ok(std::move(result));
    }

    Result<Balance> get_balance(const std::string& currency) override {
        Balance balance;
        balance.set_currency(currency.c_str());
        balance.available = 10000.0;
        balance.locked = 0.0;
        return Ok(std::move(balance));
    }

    Exchange exchange() const override { return exchange_; }
    std::string name() const override { return exchange_name(exchange_); }

private:
    Exchange exchange_;
};

// =============================================================================
// 테스트 함수들
// =============================================================================

void print_separator(const char* title) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << " " << title << "\n";
    std::cout << std::string(60, '=') << "\n";
}

void print_transfer_result(const TransferResult& result) {
    std::cout << "\n--- Transfer Result ---\n";
    std::cout << "Transfer ID: " << result.transfer_id << "\n";
    std::cout << "TX Hash: " << result.tx_hash << "\n";
    std::cout << "Status: " << transfer_status_name(result.status) << "\n";
    std::cout << "Amount: " << std::fixed << std::setprecision(4) << result.amount << "\n";
    std::cout << "Fee: " << result.fee << "\n";
    std::cout << "Elapsed: " << result.elapsed.count() / 1000 << " ms\n";
    if (result.error_message[0] != '\0') {
        std::cout << "Error: " << result.error_message << "\n";
    }
}

// 테스트 1: 송금 요청 유효성 검사
bool test_transfer_request_validation() {
    print_separator("Test 1: Transfer Request Validation");

    // 유효한 요청
    {
        TransferRequest req;
        req.from = Exchange::Binance;
        req.to = Exchange::Upbit;
        req.set_coin("XRP");
        req.amount = 100.0;
        req.to_address.set_address("rXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
        req.to_address.set_destination_tag("12345678");

        std::cout << "Valid request (Binance -> Upbit, 100 XRP): ";
        std::cout << (req.is_valid() ? "VALID" : "INVALID") << "\n";

        if (!req.is_valid()) {
            std::cout << "  ERROR: Should be valid\n";
            return false;
        }
    }

    // 같은 거래소 - 무효
    {
        TransferRequest req;
        req.from = Exchange::Binance;
        req.to = Exchange::Binance;  // Same!
        req.amount = 100.0;
        req.to_address.set_address("rXXX");
        req.to_address.set_destination_tag("123");

        std::cout << "Same exchange (Binance -> Binance): ";
        std::cout << (req.is_valid() ? "VALID" : "INVALID") << "\n";

        if (req.is_valid()) {
            std::cout << "  ERROR: Should be invalid\n";
            return false;
        }
    }

    // 수량 0 - 무효
    {
        TransferRequest req;
        req.from = Exchange::Binance;
        req.to = Exchange::Upbit;
        req.amount = 0.0;  // Zero!
        req.to_address.set_address("rXXX");
        req.to_address.set_destination_tag("123");

        std::cout << "Zero amount: ";
        std::cout << (req.is_valid() ? "VALID" : "INVALID") << "\n";

        if (req.is_valid()) {
            std::cout << "  ERROR: Should be invalid\n";
            return false;
        }
    }

    // XRP without destination tag - 무효
    {
        TransferRequest req;
        req.from = Exchange::Binance;
        req.to = Exchange::Upbit;
        req.set_coin("XRP");
        req.amount = 100.0;
        req.to_address.set_address("rXXX");
        // No destination tag!

        std::cout << "XRP without destination tag: ";
        std::cout << (req.is_valid() ? "VALID" : "INVALID") << "\n";

        if (req.is_valid()) {
            std::cout << "  ERROR: XRP requires destination tag\n";
            return false;
        }
    }

    std::cout << "\nTest Result: PASSED\n";
    return true;
}

// 테스트 2: 출금 수수료 확인
bool test_withdraw_fees() {
    print_separator("Test 2: Withdraw Fees");

    std::cout << "\nXRP Withdraw Fees by Exchange:\n";
    std::cout << "  Upbit:   " << transfer_fees::get_withdraw_fee(Exchange::Upbit) << " XRP\n";
    std::cout << "  Bithumb: " << transfer_fees::get_withdraw_fee(Exchange::Bithumb) << " XRP\n";
    std::cout << "  Binance: " << transfer_fees::get_withdraw_fee(Exchange::Binance) << " XRP\n";
    std::cout << "  MEXC:    " << transfer_fees::get_withdraw_fee(Exchange::MEXC) << " XRP\n";

    std::cout << "\nXRP Minimum Withdraw by Exchange:\n";
    std::cout << "  Upbit:   " << transfer_fees::get_min_withdraw(Exchange::Upbit) << " XRP\n";
    std::cout << "  Bithumb: " << transfer_fees::get_min_withdraw(Exchange::Bithumb) << " XRP\n";
    std::cout << "  Binance: " << transfer_fees::get_min_withdraw(Exchange::Binance) << " XRP\n";
    std::cout << "  MEXC:    " << transfer_fees::get_min_withdraw(Exchange::MEXC) << " XRP\n";

    // 검증
    bool passed = true;

    if (transfer_fees::get_withdraw_fee(Exchange::Upbit) != 0.0) {
        std::cout << "\nERROR: Upbit XRP fee should be 0\n";
        passed = false;
    }

    if (transfer_fees::get_min_withdraw(Exchange::Binance) != 20.0) {
        std::cout << "\nERROR: Binance min withdraw should be 20\n";
        passed = false;
    }

    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 3: TransferManager 생성 및 주소 등록
bool test_transfer_manager_setup() {
    print_separator("Test 3: TransferManager Setup");

    // Mock 클라이언트 생성
    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance);
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit);

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    // 입금 주소 설정
    std::map<Exchange, WithdrawAddress> addresses;

    WithdrawAddress upbit_addr;
    upbit_addr.exchange = Exchange::Upbit;
    upbit_addr.set_address("rUpbitDepositAddress12345");
    upbit_addr.set_destination_tag("88888888");
    upbit_addr.set_network("XRP");
    upbit_addr.is_whitelisted = true;
    addresses[Exchange::Upbit] = upbit_addr;

    WithdrawAddress binance_addr;
    binance_addr.exchange = Exchange::Binance;
    binance_addr.set_address("rBinanceDepositAddress123");
    binance_addr.set_destination_tag("99999999");
    binance_addr.set_network("XRP");
    binance_addr.is_whitelisted = true;
    addresses[Exchange::Binance] = binance_addr;

    // TransferManager 생성
    TransferManager manager(clients, addresses);

    std::cout << "\nRegistered deposit addresses:\n";

    auto upbit_deposit = manager.get_deposit_address(Exchange::Upbit);
    if (upbit_deposit) {
        std::cout << "  Upbit:\n";
        std::cout << "    Address: " << upbit_deposit->address << "\n";
        std::cout << "    Dest Tag: " << upbit_deposit->destination_tag << "\n";
        std::cout << "    Whitelisted: " << (upbit_deposit->is_whitelisted ? "YES" : "NO") << "\n";
    } else {
        std::cout << "  Upbit: NOT FOUND\n";
        return false;
    }

    auto binance_deposit = manager.get_deposit_address(Exchange::Binance);
    if (binance_deposit) {
        std::cout << "  Binance:\n";
        std::cout << "    Address: " << binance_deposit->address << "\n";
        std::cout << "    Dest Tag: " << binance_deposit->destination_tag << "\n";
        std::cout << "    Whitelisted: " << (binance_deposit->is_whitelisted ? "YES" : "NO") << "\n";
    } else {
        std::cout << "  Binance: NOT FOUND\n";
        return false;
    }

    // 화이트리스트 확인
    std::cout << "\nWhitelist check:\n";
    std::cout << "  Binance -> Upbit: "
              << (manager.is_whitelisted(Exchange::Binance, Exchange::Upbit) ? "YES" : "NO") << "\n";
    std::cout << "  Upbit -> Binance: "
              << (manager.is_whitelisted(Exchange::Upbit, Exchange::Binance) ? "YES" : "NO") << "\n";
    std::cout << "  Binance -> MEXC: "
              << (manager.is_whitelisted(Exchange::Binance, Exchange::MEXC) ? "YES" : "NO") << "\n";

    std::cout << "\nTest Result: PASSED\n";
    return true;
}

// 테스트 4: Dry Run 송금
bool test_dry_run_transfer() {
    print_separator("Test 4: Dry Run Transfer");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance);
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit);

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    std::map<Exchange, WithdrawAddress> addresses;
    WithdrawAddress upbit_addr;
    upbit_addr.exchange = Exchange::Upbit;
    upbit_addr.set_address("rUpbitAddress");
    upbit_addr.set_destination_tag("12345678");
    addresses[Exchange::Upbit] = upbit_addr;

    TransferManager manager(clients, addresses);
    manager.set_dry_run(true);  // Dry run 모드 활성화

    // 송금 요청 생성
    TransferRequest request;
    request.from = Exchange::Binance;
    request.to = Exchange::Upbit;
    request.set_coin("XRP");
    request.amount = 500.0;
    request.to_address = upbit_addr;
    request.set_request_id_auto();

    std::cout << "\nInitiating transfer (DRY RUN):\n";
    std::cout << "  From: " << exchange_name(request.from) << "\n";
    std::cout << "  To: " << exchange_name(request.to) << "\n";
    std::cout << "  Amount: " << request.amount << " " << request.coin << "\n";
    std::cout << "  Destination Tag: " << request.to_address.destination_tag << "\n";

    auto result = manager.initiate_sync(request);

    if (!result.has_value()) {
        std::cout << "\nERROR: " << result.error().message << "\n";
        return false;
    }

    print_transfer_result(result.value());

    // 검증
    bool passed = result.value().status == TransferStatus::Completed;

    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 5: 최소 출금 수량 미달
bool test_minimum_amount_check() {
    print_separator("Test 5: Minimum Amount Check");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance);

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;

    std::map<Exchange, WithdrawAddress> addresses;
    WithdrawAddress upbit_addr;
    upbit_addr.set_address("rUpbitAddress");
    upbit_addr.set_destination_tag("12345678");
    addresses[Exchange::Upbit] = upbit_addr;

    TransferManager manager(clients, addresses);
    manager.set_dry_run(true);

    // 최소 출금 수량 미달 요청
    TransferRequest request;
    request.from = Exchange::Binance;
    request.to = Exchange::Upbit;
    request.set_coin("XRP");
    request.amount = 5.0;  // Binance minimum is 20 XRP
    request.to_address = upbit_addr;

    std::cout << "\nInitiating transfer with insufficient amount:\n";
    std::cout << "  Amount: " << request.amount << " XRP\n";
    std::cout << "  Minimum: " << transfer_fees::get_min_withdraw(Exchange::Binance) << " XRP\n";

    auto result = manager.initiate_sync(request);

    if (!result.has_value()) {
        std::cout << "\nUnexpected error: " << result.error().message << "\n";
        return false;
    }

    print_transfer_result(result.value());

    // 실패해야 함
    bool passed = result.value().status == TransferStatus::Failed;

    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 6: 비동기 송금
bool test_async_transfer() {
    print_separator("Test 6: Async Transfer");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance);
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit);

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    std::map<Exchange, WithdrawAddress> addresses;
    WithdrawAddress upbit_addr;
    upbit_addr.set_address("rUpbitAddress");
    upbit_addr.set_destination_tag("12345678");
    addresses[Exchange::Upbit] = upbit_addr;

    TransferManager manager(clients, addresses);
    manager.set_dry_run(true);

    TransferRequest request;
    request.from = Exchange::Binance;
    request.to = Exchange::Upbit;
    request.set_coin("XRP");
    request.amount = 100.0;
    request.to_address = upbit_addr;

    std::cout << "\nLaunching async transfer...\n";

    auto start = std::chrono::steady_clock::now();
    auto future = manager.initiate(request);

    std::cout << "Async call returned, doing other work...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "Waiting for result...\n";
    auto result = future.get();
    auto end = std::chrono::steady_clock::now();

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Total time: " << total_ms << " ms\n";

    if (result.has_value()) {
        print_transfer_result(result.value());
    }

    bool passed = result.has_value() && result.value().is_completed();
    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 7: 통계 확인
bool test_statistics() {
    print_separator("Test 7: Statistics");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance);
    auto upbit = std::make_shared<MockOrderClient>(Exchange::Upbit);

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;
    clients[Exchange::Upbit] = upbit;

    std::map<Exchange, WithdrawAddress> addresses;
    WithdrawAddress upbit_addr;
    upbit_addr.set_address("rUpbitAddress");
    upbit_addr.set_destination_tag("12345678");
    addresses[Exchange::Upbit] = upbit_addr;

    TransferManager manager(clients, addresses);
    manager.set_dry_run(true);

    // 여러 번 송금 실행
    for (int i = 0; i < 5; ++i) {
        TransferRequest request;
        request.from = Exchange::Binance;
        request.to = Exchange::Upbit;
        request.set_coin("XRP");
        request.amount = 100.0 + i * 10;
        request.to_address = upbit_addr;

        manager.initiate_sync(request);
    }

    // 실패 케이스 추가
    {
        TransferRequest request;
        request.from = Exchange::Binance;
        request.to = Exchange::Upbit;
        request.set_coin("XRP");
        request.amount = 5.0;  // Below minimum
        request.to_address = upbit_addr;

        manager.initiate_sync(request);
    }

    const auto& stats = manager.stats();

    std::cout << "\nTransfer Statistics:\n";
    std::cout << "  Total Transfers: " << stats.total_transfers.load() << "\n";
    std::cout << "  Successful: " << stats.successful.load() << "\n";
    std::cout << "  Failed: " << stats.failed.load() << "\n";
    std::cout << "  Timeout: " << stats.timeout.load() << "\n";
    std::cout << "  Success Rate: " << std::fixed << std::setprecision(1)
              << stats.success_rate() << "%\n";

    bool passed = stats.total_transfers.load() == 6 &&
                  stats.successful.load() == 5 &&
                  stats.failed.load() == 1;

    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// 테스트 8: 상태 콜백
bool test_status_callback() {
    print_separator("Test 8: Status Callback");

    auto binance = std::make_shared<MockOrderClient>(Exchange::Binance);

    std::map<Exchange, std::shared_ptr<OrderClientBase>> clients;
    clients[Exchange::Binance] = binance;

    std::map<Exchange, WithdrawAddress> addresses;
    WithdrawAddress upbit_addr;
    upbit_addr.set_address("rUpbitAddress");
    upbit_addr.set_destination_tag("12345678");
    addresses[Exchange::Upbit] = upbit_addr;

    TransferManager manager(clients, addresses);
    manager.set_dry_run(true);

    bool callback_called = false;
    TransferResult callback_result;

    std::cout << "\nSetting up callback...\n";

    manager.initiate_with_callback(
        [&]() {
            TransferRequest request;
            request.from = Exchange::Binance;
            request.to = Exchange::Upbit;
            request.set_coin("XRP");
            request.amount = 200.0;
            request.to_address = upbit_addr;
            return request;
        }(),
        [&](const TransferResult& result) {
            callback_called = true;
            callback_result = result;
            std::cout << "[Callback] Transfer completed!\n";
            std::cout << "  Status: " << transfer_status_name(result.status) << "\n";
        }
    );

    // 콜백 대기
    std::cout << "Waiting for callback...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bool passed = callback_called && callback_result.is_completed();
    std::cout << "\nTest Result: " << (passed ? "PASSED" : "FAILED") << "\n";
    return passed;
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << " TASK_09: Transfer Test\n";
    std::cout << " Exchange Transfer Manager\n";
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

    run_test("Transfer Request Validation", test_transfer_request_validation);
    run_test("Withdraw Fees", test_withdraw_fees);
    run_test("TransferManager Setup", test_transfer_manager_setup);
    run_test("Dry Run Transfer", test_dry_run_transfer);
    run_test("Minimum Amount Check", test_minimum_amount_check);
    run_test("Async Transfer", test_async_transfer);
    run_test("Statistics", test_statistics);
    run_test("Status Callback", test_status_callback);

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
