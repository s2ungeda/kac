#pragma once

/**
 * Multi Account Manager (TASK_17)
 *
 * 거래소당 복수 계정 관리
 * - 계정 등록/삭제
 * - 가중치 기반 계정 선택
 * - 잔고 통합 조회
 * - 라운드 로빈/가중치 기반 분배
 */

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/error.hpp"
#include "arbitrage/common/spin_wait.hpp"

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <chrono>
#include <functional>
#include <atomic>

namespace arbitrage {

// =============================================================================
// 계정 상태
// =============================================================================
enum class AccountStatus : uint8_t {
    Active,      // 정상 사용 가능
    Disabled,    // 사용자가 비활성화
    RateLimited, // API 제한 상태
    Error,       // 오류 발생
    Maintenance  // 점검 중
};

// =============================================================================
// 계정 정보
// =============================================================================
struct Account {
    std::string id;                    // 계정 고유 ID (예: "upbit_main", "binance_sub1")
    Exchange exchange;                 // 거래소
    std::string label;                 // 표시 이름 (예: "메인 계정")

    // API 인증 정보 (SecretsManager에서 관리하는 키 이름)
    std::string api_key_ref;           // secrets에 저장된 API key 참조 이름
    std::string api_secret_ref;        // secrets에 저장된 API secret 참조 이름

    // 설정
    bool enabled{true};                // 활성화 여부
    double weight{1.0};                // 주문 분배 가중치 (0.0 ~ 1.0)
    double max_order_amount{0.0};      // 최대 주문 금액 (0 = 무제한)

    // 상태
    AccountStatus status{AccountStatus::Active};
    std::string status_message;

    // 메타데이터
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_used_at;
    uint64_t order_count{0};           // 주문 횟수
    uint64_t error_count{0};           // 오류 횟수

    // 잔고 캐시
    std::map<std::string, double> balances;  // symbol -> amount
    std::chrono::system_clock::time_point balance_updated_at;

    bool is_active() const {
        return enabled && status == AccountStatus::Active;
    }
};

// =============================================================================
// 계정 선택 전략
// =============================================================================
enum class AccountSelectionStrategy : uint8_t {
    RoundRobin,      // 순차 선택
    WeightedRandom,  // 가중치 기반 랜덤
    LeastUsed,       // 사용 횟수 적은 순
    HighestBalance,  // 잔고 많은 순
    LowestLatency    // 응답 속도 빠른 순
};

// =============================================================================
// 계정 관리자
// =============================================================================
class AccountManager {
public:
    // 콜백 타입
    using BalanceCallback = std::function<std::map<std::string, double>(const Account&)>;
    using AccountChangeCallback = std::function<void(const Account&, const std::string& event)>;

    AccountManager();
    ~AccountManager() = default;

    // 복사/이동 금지
    AccountManager(const AccountManager&) = delete;
    AccountManager& operator=(const AccountManager&) = delete;

    // =========================================================================
    // 계정 관리
    // =========================================================================

    /**
     * 계정 추가
     * @param account 계정 정보
     * @return 성공 여부
     */
    Result<void> add_account(const Account& account);

    /**
     * 계정 삭제
     * @param account_id 계정 ID
     * @return 성공 여부
     */
    Result<void> remove_account(const std::string& account_id);

    /**
     * 계정 업데이트
     * @param account 업데이트된 계정 정보
     * @return 성공 여부
     */
    Result<void> update_account(const Account& account);

    /**
     * 계정 조회
     * @param account_id 계정 ID
     * @return 계정 정보
     */
    std::optional<Account> get_account(const std::string& account_id) const;

    /**
     * 거래소별 계정 목록 조회
     * @param exchange 거래소
     * @return 계정 목록
     */
    std::vector<Account> get_accounts(Exchange exchange) const;

    /**
     * 활성화된 계정만 조회
     */
    std::vector<Account> get_active_accounts(Exchange exchange) const;

    /**
     * 전체 계정 목록 조회
     */
    std::vector<Account> get_all_accounts() const;

    /**
     * 계정 수
     */
    size_t count() const;
    size_t count(Exchange exchange) const;

    // =========================================================================
    // 계정 선택
    // =========================================================================

    /**
     * 최적 계정 선택
     * @param exchange 거래소
     * @param required_balance 필요 잔고 (선택)
     * @param symbol 거래 심볼 (잔고 확인용)
     * @return 선택된 계정
     */
    std::optional<Account> select_account(
        Exchange exchange,
        double required_balance = 0.0,
        const std::string& symbol = ""
    );

    /**
     * 선택 전략 설정
     */
    void set_selection_strategy(AccountSelectionStrategy strategy) {
        strategy_ = strategy;
    }

    AccountSelectionStrategy get_selection_strategy() const {
        return strategy_;
    }

    // =========================================================================
    // 잔고 관리
    // =========================================================================

    /**
     * 계정 잔고 업데이트
     * @param account_id 계정 ID
     * @param balances 심볼별 잔고
     */
    void update_balance(
        const std::string& account_id,
        const std::map<std::string, double>& balances
    );

    /**
     * 거래소 전체 잔고 조회 (모든 계정 합산)
     * @param exchange 거래소
     * @return 심볼별 총 잔고
     */
    std::map<std::string, double> get_total_balance(Exchange exchange) const;

    /**
     * 특정 심볼 잔고 조회 (모든 계정 합산)
     */
    double get_total_balance(Exchange exchange, const std::string& symbol) const;

    /**
     * 잔고 새로고침 콜백 설정
     */
    void set_balance_callback(BalanceCallback callback) {
        balance_callback_ = std::move(callback);
    }

    /**
     * 잔고 새로고침 실행
     */
    void refresh_balances(Exchange exchange);
    void refresh_all_balances();

    // =========================================================================
    // 상태 관리
    // =========================================================================

    /**
     * 계정 활성화/비활성화
     */
    void enable_account(const std::string& account_id);
    void disable_account(const std::string& account_id);

    /**
     * 계정 상태 업데이트
     */
    void set_account_status(
        const std::string& account_id,
        AccountStatus status,
        const std::string& message = ""
    );

    /**
     * 주문 사용 기록
     */
    void record_order(const std::string& account_id);

    /**
     * 오류 기록
     */
    void record_error(const std::string& account_id);

    // =========================================================================
    // 이벤트
    // =========================================================================

    /**
     * 계정 변경 콜백 설정
     */
    void on_account_change(AccountChangeCallback callback);

    // =========================================================================
    // 통계
    // =========================================================================

    struct Stats {
        std::atomic<uint64_t> total_orders{0};
        std::atomic<uint64_t> total_errors{0};
        std::atomic<uint64_t> selection_count{0};

        void reset() {
            total_orders = 0;
            total_errors = 0;
            selection_count = 0;
        }
    };

    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

    // =========================================================================
    // 설정 파일 저장/로드
    // =========================================================================

    /**
     * 설정 파일로 저장 (API 키 제외)
     */
    Result<void> save_to_file(const std::string& path) const;

    /**
     * 설정 파일에서 로드
     */
    Result<void> load_from_file(const std::string& path);

private:
    // 라운드 로빈 인덱스 증가
    size_t next_round_robin_index(Exchange exchange);

    // 가중치 기반 랜덤 선택
    std::optional<Account> select_weighted_random(
        const std::vector<Account>& candidates
    );

    // 변경 이벤트 발생
    void notify_change(const Account& account, const std::string& event);

    // 멤버 변수
    mutable RWSpinLock mutex_;
    std::map<std::string, Account> accounts_;  // account_id -> Account

    AccountSelectionStrategy strategy_{AccountSelectionStrategy::RoundRobin};
    std::map<Exchange, size_t> round_robin_index_;

    BalanceCallback balance_callback_;
    std::vector<AccountChangeCallback> change_callbacks_;

    mutable Stats stats_;
};

// =============================================================================
// 글로벌 인스턴스 접근자
// =============================================================================
AccountManager& account_manager();

}  // namespace arbitrage
