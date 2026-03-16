#pragma once

/**
 * CLI Tool Commands (TASK_27)
 *
 * 시스템 관리 및 디버깅용 명령줄 도구
 * - TCP 서버와 통신
 * - 상태 조회
 * - 수동 주문
 * - 킬스위치 제어
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace arbitrage::cli {

// =============================================================================
// CLI 설정
// =============================================================================

struct CLIConfig {
    std::string server_host{"localhost"};
    int server_port{9800};
    int connect_timeout_ms{5000};
    int read_timeout_ms{10000};
    std::string auth_token;
    bool verbose{false};
    bool color_output{true};
};

// =============================================================================
// 응답 구조체
// =============================================================================

/**
 * 시스템 상태 응답
 */
struct SystemStatusResponse {
    bool running{false};
    std::string uptime;
    int active_connections{0};
    int pending_orders{0};
    bool kill_switch_active{false};
    double daily_pnl{0.0};
    int daily_trades{0};
    double remaining_limit{0.0};
    std::string strategy_state;
};

/**
 * 김프 매트릭스 응답
 */
struct PremiumResponse {
    struct Entry {
        std::string buy_exchange;
        std::string sell_exchange;
        double premium_pct{0.0};
        double buy_price{0.0};
        double sell_price{0.0};
        std::string currency;
    };
    std::vector<Entry> matrix;
    double fx_rate{0.0};
    std::chrono::system_clock::time_point timestamp;
};

/**
 * 잔고 응답
 */
struct BalanceResponse {
    struct ExchangeBalance {
        std::string exchange;
        double xrp{0.0};
        double usdt{0.0};
        double krw{0.0};
    };
    std::vector<ExchangeBalance> balances;
    double total_value_krw{0.0};
};

/**
 * 주문 상태 응답
 */
struct OrderStatusResponse {
    std::string order_id;
    std::string exchange;
    std::string side;
    std::string type;
    double quantity{0.0};
    double price{0.0};
    double filled_qty{0.0};
    std::string status;
    std::string created_at;
};

/**
 * 거래 내역 응답
 */
struct TradeHistoryResponse {
    struct Trade {
        std::string id;
        std::string buy_exchange;
        std::string sell_exchange;
        double quantity{0.0};
        double buy_price{0.0};
        double sell_price{0.0};
        double pnl{0.0};
        std::string timestamp;
    };
    std::vector<Trade> trades;
    double total_pnl{0.0};
    int total_count{0};
};

/**
 * 헬스 체크 응답
 */
struct HealthResponse {
    struct Component {
        std::string name;
        std::string status;  // "Healthy", "Degraded", "Unhealthy"
        std::string message;
    };
    std::vector<Component> components;
    std::string overall;
    double cpu_percent{0.0};
    uint64_t memory_bytes{0};
};

/**
 * 명령 응답
 */
struct CommandResponse {
    bool success{false};
    std::string message;
    std::string error;
};

// =============================================================================
// CLI 클래스
// =============================================================================

/**
 * CLI 클라이언트
 *
 * TCP 서버와 통신하여 명령을 실행
 */
class CLI {
public:
    explicit CLI(const CLIConfig& config = {});
    ~CLI();

    CLI(const CLI&) = delete;
    CLI& operator=(const CLI&) = delete;

    // =========================================================================
    // 연결
    // =========================================================================

    /**
     * 서버에 연결
     */
    bool connect();
    bool connect(const std::string& host, int port);

    /**
     * 연결 해제
     */
    void disconnect();

    /**
     * 연결 상태 확인
     */
    bool is_connected() const;

    // =========================================================================
    // 조회 명령
    // =========================================================================

    /**
     * 시스템 상태 조회
     */
    std::optional<SystemStatusResponse> status();

    /**
     * 김프 매트릭스 조회
     */
    std::optional<PremiumResponse> premium();

    /**
     * 잔고 조회
     */
    std::optional<BalanceResponse> balance();

    /**
     * 주문 상태 조회
     */
    std::optional<OrderStatusResponse> order_status(const std::string& order_id);

    /**
     * 거래 내역 조회
     */
    std::optional<TradeHistoryResponse> history(int count = 10);

    /**
     * 헬스 체크
     */
    std::optional<HealthResponse> health();

    // =========================================================================
    // 제어 명령
    // =========================================================================

    /**
     * 수동 주문
     */
    CommandResponse order(
        const std::string& exchange,
        const std::string& side,
        double quantity,
        double price = 0.0  // 0 = 시장가
    );

    /**
     * 주문 취소
     */
    CommandResponse cancel(const std::string& order_id);

    /**
     * 킬스위치 활성화
     */
    CommandResponse kill(const std::string& reason = "Manual CLI kill");

    /**
     * 킬스위치 해제
     */
    CommandResponse resume();

    /**
     * 전략 시작
     */
    CommandResponse start_strategy(const std::string& strategy_id = "");

    /**
     * 전략 중지
     */
    CommandResponse stop_strategy(const std::string& strategy_id = "");

    /**
     * 설정 변경
     */
    CommandResponse config_set(const std::string& key, const std::string& value);

    /**
     * 설정 조회
     */
    std::optional<std::string> config_get(const std::string& key);

    // =========================================================================
    // 출력 헬퍼
    // =========================================================================

    /**
     * 상태 출력 (터미널)
     */
    void print_status(const SystemStatusResponse& status);

    /**
     * 김프 매트릭스 출력
     */
    void print_premium(const PremiumResponse& premium);

    /**
     * 잔고 출력
     */
    void print_balance(const BalanceResponse& balance);

    /**
     * 거래 내역 출력
     */
    void print_history(const TradeHistoryResponse& history);

    /**
     * 헬스 체크 출력
     */
    void print_health(const HealthResponse& health);

    /**
     * 명령 결과 출력
     */
    void print_response(const CommandResponse& response);

    // =========================================================================
    // 유틸리티
    // =========================================================================

    /**
     * 마지막 에러 메시지
     */
    const std::string& last_error() const { return last_error_; }

    /**
     * 설정 변경
     */
    void set_config(const CLIConfig& config);

private:
    /**
     * 메시지 전송
     */
    bool send_message(uint8_t type, const std::string& payload);
    bool send_message(uint8_t type, const std::vector<uint8_t>& payload);

    /**
     * 응답 수신
     */
    std::optional<std::pair<uint8_t, std::vector<uint8_t>>> receive_response();

    /**
     * JSON 파싱 헬퍼
     */
    std::string make_json(const std::vector<std::pair<std::string, std::string>>& fields);

    /**
     * 컬러 출력 헬퍼
     */
    std::string color(const std::string& text, const std::string& color_code);
    std::string green(const std::string& text);
    std::string red(const std::string& text);
    std::string yellow(const std::string& text);
    std::string cyan(const std::string& text);
    std::string bold(const std::string& text);

private:
    CLIConfig config_;
    int socket_fd_{-1};
    std::string last_error_;
};

// =============================================================================
// 유틸리티 함수
// =============================================================================

/**
 * 사용법 출력
 */
void print_usage(const char* program_name);

/**
 * 버전 출력
 */
void print_version();

/**
 * 포맷된 숫자 출력 (천단위 콤마)
 */
std::string format_number(double value, int precision = 2);

/**
 * 포맷된 KRW 출력
 */
std::string format_krw(double value);

/**
 * 포맷된 퍼센트 출력
 */
std::string format_percent(double value);

/**
 * 상대 시간 출력 (예: "2시간 전")
 */
std::string format_relative_time(const std::chrono::system_clock::time_point& time);

}  // namespace arbitrage::cli
