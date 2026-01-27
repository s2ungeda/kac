#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/error.hpp"
#include "arbitrage/exchange/order_base.hpp"
#include <string>
#include <optional>
#include <map>
#include <memory>
#include <future>
#include <atomic>
#include <functional>
#include <chrono>

namespace arbitrage {

// =============================================================================
// 송금 상태
// =============================================================================
enum class TransferStatus : uint8_t {
    Pending,       // 출금 요청됨, 대기 중
    Processing,    // 블록체인 처리 중
    Completed,     // 입금 완료
    Failed,        // 실패
    Timeout,       // 타임아웃
    Cancelled      // 취소됨
};

// 상태 이름 변환
constexpr const char* transfer_status_name(TransferStatus status) {
    switch (status) {
        case TransferStatus::Pending:    return "Pending";
        case TransferStatus::Processing: return "Processing";
        case TransferStatus::Completed:  return "Completed";
        case TransferStatus::Failed:     return "Failed";
        case TransferStatus::Timeout:    return "Timeout";
        case TransferStatus::Cancelled:  return "Cancelled";
        default:                         return "Unknown";
    }
}

// =============================================================================
// 출금 주소 정보
// =============================================================================
struct WithdrawAddress {
    Exchange exchange{Exchange::Upbit};          // 대상 거래소
    char address[64]{};                          // 지갑 주소
    char destination_tag[32]{};                  // XRP Destination Tag (필수!)
    char network[16]{};                          // 네트워크 (예: XRP)
    bool is_whitelisted{false};                  // 화이트리스트 등록 여부

    WithdrawAddress() {
        address[0] = '\0';
        destination_tag[0] = '\0';
        network[0] = '\0';
    }

    void set_address(const char* addr) {
        std::strncpy(address, addr, sizeof(address) - 1);
        address[sizeof(address) - 1] = '\0';
    }

    void set_address(const std::string& addr) {
        set_address(addr.c_str());
    }

    void set_destination_tag(const char* tag) {
        std::strncpy(destination_tag, tag, sizeof(destination_tag) - 1);
        destination_tag[sizeof(destination_tag) - 1] = '\0';
    }

    void set_destination_tag(const std::string& tag) {
        set_destination_tag(tag.c_str());
    }

    void set_network(const char* net) {
        std::strncpy(network, net, sizeof(network) - 1);
        network[sizeof(network) - 1] = '\0';
    }

    bool has_destination_tag() const {
        return destination_tag[0] != '\0';
    }
};

// =============================================================================
// 송금 요청
// =============================================================================
struct alignas(CACHE_LINE_SIZE) TransferRequest {
    Exchange from{Exchange::Binance};            // 출금 거래소
    Exchange to{Exchange::Upbit};                // 입금 거래소
    char coin[16]{"XRP"};                        // 코인 (기본: XRP)
    double amount{0.0};                          // 송금 수량
    WithdrawAddress to_address{};                // 입금 주소 정보
    int64_t request_id{0};                       // 요청 ID

    void set_coin(const char* c) {
        std::strncpy(coin, c, sizeof(coin) - 1);
        coin[sizeof(coin) - 1] = '\0';
    }

    void set_request_id_auto() {
        auto now = std::chrono::steady_clock::now();
        request_id = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
    }

    bool is_valid() const {
        // 출금/입금 거래소가 달라야 함
        if (from == to) return false;
        // 수량이 양수여야 함
        if (amount <= 0) return false;
        // 주소가 있어야 함
        if (to_address.address[0] == '\0') return false;
        // XRP는 destination tag 필수
        if (std::strcmp(coin, "XRP") == 0 && !to_address.has_destination_tag()) {
            return false;
        }
        return true;
    }
};

// =============================================================================
// 송금 결과
// =============================================================================
struct TransferResult {
    char transfer_id[64];                        // 출금 ID (거래소 발급)
    char tx_hash[128];                           // 블록체인 트랜잭션 해시
    TransferStatus status{TransferStatus::Pending};
    double amount{0.0};                          // 실제 송금 수량
    double fee{0.0};                             // 출금 수수료
    Duration elapsed{0};                         // 경과 시간
    char error_message[MAX_MESSAGE_LEN];         // 에러 메시지
    SteadyTimePoint start_time;                  // 시작 시간
    SteadyTimePoint end_time;                    // 완료 시간

    void set_transfer_id(const char* id) {
        std::strncpy(transfer_id, id, sizeof(transfer_id) - 1);
        transfer_id[sizeof(transfer_id) - 1] = '\0';
    }

    void set_transfer_id(const std::string& id) {
        set_transfer_id(id.c_str());
    }

    void set_tx_hash(const char* hash) {
        std::strncpy(tx_hash, hash, sizeof(tx_hash) - 1);
        tx_hash[sizeof(tx_hash) - 1] = '\0';
    }

    void set_tx_hash(const std::string& hash) {
        set_tx_hash(hash.c_str());
    }

    void set_error(const char* msg) {
        std::strncpy(error_message, msg, sizeof(error_message) - 1);
        error_message[sizeof(error_message) - 1] = '\0';
        status = TransferStatus::Failed;
    }

    void set_error(const std::string& msg) {
        set_error(msg.c_str());
    }

    bool is_completed() const {
        return status == TransferStatus::Completed;
    }

    bool is_failed() const {
        return status == TransferStatus::Failed ||
               status == TransferStatus::Timeout ||
               status == TransferStatus::Cancelled;
    }

    bool is_pending() const {
        return status == TransferStatus::Pending ||
               status == TransferStatus::Processing;
    }

    void calculate_elapsed() {
        elapsed = std::chrono::duration_cast<Duration>(end_time - start_time);
    }
};

// =============================================================================
// 입금 기록
// =============================================================================
struct DepositRecord {
    char tx_hash[128];
    char coin[16];
    double amount{0.0};
    int64_t timestamp_us{0};
    bool confirmed{false};

    void set_tx_hash(const char* hash) {
        std::strncpy(tx_hash, hash, sizeof(tx_hash) - 1);
        tx_hash[sizeof(tx_hash) - 1] = '\0';
    }

    void set_coin(const char* c) {
        std::strncpy(coin, c, sizeof(coin) - 1);
        coin[sizeof(coin) - 1] = '\0';
    }
};

// =============================================================================
// 송금 관리자 콜백
// =============================================================================
using TransferCallback = std::function<void(const TransferResult&)>;
using TransferStatusCallback = std::function<void(const std::string& transfer_id, TransferStatus status)>;

// =============================================================================
// 송금 관리자
// =============================================================================
// XRP 거래소 간 송금 관리
//
// 핵심 기능:
// 1. 출금 API 연동 (Upbit, Binance, Bithumb, MEXC)
// 2. Destination Tag 처리 (XRP 필수)
// 3. 송금 상태 추적 (폴링)
// 4. 입금 확인
// 5. 타임아웃 처리
// =============================================================================
class TransferManager {
public:
    // 생성자
    // @param order_clients 거래소별 주문 클라이언트 (출금 API용)
    // @param addresses 거래소별 입금 주소
    TransferManager(
        std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients,
        std::map<Exchange, WithdrawAddress> deposit_addresses = {}
    );

    ~TransferManager();

    // 복사/이동 금지
    TransferManager(const TransferManager&) = delete;
    TransferManager& operator=(const TransferManager&) = delete;

    // =========================================================================
    // 주소 관리
    // =========================================================================

    // 입금 주소 등록
    void register_deposit_address(Exchange ex, const WithdrawAddress& address);

    // 입금 주소 조회
    std::optional<WithdrawAddress> get_deposit_address(Exchange ex) const;

    // 출금 주소 화이트리스트 확인
    bool is_whitelisted(Exchange from, Exchange to) const;

    // =========================================================================
    // 송금 실행
    // =========================================================================

    // 송금 시작 (비동기)
    // @param request 송금 요청
    // @return 송금 결과 future
    std::future<Result<TransferResult>> initiate(const TransferRequest& request);

    // 송금 시작 (동기)
    Result<TransferResult> initiate_sync(const TransferRequest& request);

    // 콜백 방식 송금
    void initiate_with_callback(
        const TransferRequest& request,
        TransferCallback callback
    );

    // =========================================================================
    // 상태 확인
    // =========================================================================

    // 출금 상태 확인
    // @param exchange 출금 거래소
    // @param transfer_id 출금 ID
    // @return 현재 상태
    std::future<Result<TransferStatus>> check_withdraw_status(
        Exchange exchange,
        const std::string& transfer_id
    );

    // 입금 확인
    // @param exchange 입금 거래소
    // @param tx_hash 트랜잭션 해시
    // @return 입금 확인 여부
    std::future<Result<bool>> check_deposit(
        Exchange exchange,
        const std::string& tx_hash
    );

    // =========================================================================
    // 완료 대기
    // =========================================================================

    // 송금 완료 대기 (폴링)
    // @param exchange 출금 거래소
    // @param transfer_id 출금 ID
    // @param timeout 타임아웃 (기본 30분)
    // @param poll_interval 폴링 간격 (기본 10초)
    // @return 최종 결과
    std::future<Result<TransferResult>> wait_completion(
        Exchange exchange,
        const std::string& transfer_id,
        Duration timeout = std::chrono::minutes(30),
        Duration poll_interval = std::chrono::seconds(10)
    );

    // 상태 변경 콜백 설정
    void set_status_callback(TransferStatusCallback callback) {
        status_callback_ = std::move(callback);
    }

    // =========================================================================
    // 설정
    // =========================================================================

    // 기본 타임아웃 설정
    void set_default_timeout(Duration timeout) { default_timeout_ = timeout; }
    Duration default_timeout() const { return default_timeout_; }

    // 폴링 간격 설정
    void set_poll_interval(Duration interval) { poll_interval_ = interval; }
    Duration poll_interval() const { return poll_interval_; }

    // dry run 모드 (테스트용)
    void set_dry_run(bool enabled) { dry_run_ = enabled; }
    bool is_dry_run() const { return dry_run_; }

    // =========================================================================
    // 통계
    // =========================================================================

    struct Stats {
        std::atomic<uint64_t> total_transfers{0};
        std::atomic<uint64_t> successful{0};
        std::atomic<uint64_t> failed{0};
        std::atomic<uint64_t> timeout{0};
        std::atomic<int64_t> total_elapsed_us{0};

        double success_rate() const {
            uint64_t total = total_transfers.load();
            return total > 0 ?
                static_cast<double>(successful.load()) / total * 100.0 : 0.0;
        }

        double avg_elapsed_minutes() const {
            uint64_t success = successful.load();
            return success > 0 ?
                static_cast<double>(total_elapsed_us.load()) / success / 60000000.0 : 0.0;
        }

        void reset() {
            total_transfers = 0;
            successful = 0;
            failed = 0;
            timeout = 0;
            total_elapsed_us = 0;
        }
    };

    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

private:
    // 거래소별 출금 API 호출
    Result<std::string> withdraw(Exchange ex, const TransferRequest& request);

    // 거래소별 출금 상태 조회
    Result<TransferStatus> get_withdraw_status(Exchange ex, const std::string& transfer_id);

    // 거래소별 입금 확인
    Result<bool> verify_deposit(Exchange ex, const std::string& tx_hash);

    // 거래소별 트랜잭션 해시 조회
    Result<std::string> get_tx_hash(Exchange ex, const std::string& transfer_id);

    // 통계 업데이트
    void record_result(const TransferResult& result);

    // 멤버 변수
    std::map<Exchange, std::shared_ptr<OrderClientBase>> order_clients_;
    std::map<Exchange, WithdrawAddress> deposit_addresses_;
    Stats stats_;

    Duration default_timeout_{std::chrono::minutes(30)};
    Duration poll_interval_{std::chrono::seconds(10)};
    bool dry_run_{false};

    TransferStatusCallback status_callback_;
};

// =============================================================================
// XRP 송금 수수료 (거래소별)
// =============================================================================
namespace transfer_fees {

// XRP 출금 수수료 (2024 기준, 변동 가능)
constexpr double UPBIT_XRP_FEE = 0.0;      // 무료
constexpr double BITHUMB_XRP_FEE = 0.0;    // 무료
constexpr double BINANCE_XRP_FEE = 0.25;   // 0.25 XRP
constexpr double MEXC_XRP_FEE = 0.25;      // 0.25 XRP

inline double get_withdraw_fee(Exchange ex) {
    switch (ex) {
        case Exchange::Upbit:   return UPBIT_XRP_FEE;
        case Exchange::Bithumb: return BITHUMB_XRP_FEE;
        case Exchange::Binance: return BINANCE_XRP_FEE;
        case Exchange::MEXC:    return MEXC_XRP_FEE;
        default:                return 0.0;
    }
}

// XRP 최소 출금 수량
constexpr double UPBIT_XRP_MIN = 21.0;
constexpr double BITHUMB_XRP_MIN = 25.0;
constexpr double BINANCE_XRP_MIN = 20.0;
constexpr double MEXC_XRP_MIN = 20.0;

inline double get_min_withdraw(Exchange ex) {
    switch (ex) {
        case Exchange::Upbit:   return UPBIT_XRP_MIN;
        case Exchange::Bithumb: return BITHUMB_XRP_MIN;
        case Exchange::Binance: return BINANCE_XRP_MIN;
        case Exchange::MEXC:    return MEXC_XRP_MIN;
        default:                return 20.0;
    }
}

}  // namespace transfer_fees

}  // namespace arbitrage
