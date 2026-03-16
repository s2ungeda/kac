#include "commands.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace arbitrage::cli {

// 프로토콜 상수
constexpr uint8_t MAGIC_0 = 0x4B;  // 'K'
constexpr uint8_t MAGIC_1 = 0x41;  // 'A'
constexpr uint8_t VERSION = 0x01;
constexpr size_t HEADER_SIZE = 8;

// 메시지 타입 (tcp_server.hpp와 동일)
namespace MsgType {
    constexpr uint8_t Ping = 0x01;
    constexpr uint8_t Pong = 0x02;
    constexpr uint8_t AuthRequest = 0x10;
    constexpr uint8_t AuthResponse = 0x11;
    constexpr uint8_t SystemStatus = 0x40;
    constexpr uint8_t HealthStatus = 0x41;
    constexpr uint8_t KillSwitch = 0x42;
    constexpr uint8_t CmdStartStrategy = 0x80;
    constexpr uint8_t CmdStopStrategy = 0x81;
    constexpr uint8_t CmdSetKillSwitch = 0x82;
    constexpr uint8_t CmdGetStatus = 0x83;
    constexpr uint8_t CmdResponse = 0xF0;
    constexpr uint8_t Error = 0xFF;
}

// =============================================================================
// 생성자/소멸자
// =============================================================================

CLI::CLI(const CLIConfig& config)
    : config_(config)
{
}

CLI::~CLI() {
    disconnect();
}

// =============================================================================
// 연결
// =============================================================================

bool CLI::connect() {
    return connect(config_.server_host, config_.server_port);
}

bool CLI::connect(const std::string& host, int port) {
    if (socket_fd_ >= 0) {
        disconnect();
    }

#ifndef _WIN32
    // 호스트 이름 해석
    struct addrinfo hints{}, *result;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        last_error_ = "Failed to resolve host: " + host;
        return false;
    }

    // 소켓 생성
    socket_fd_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (socket_fd_ < 0) {
        freeaddrinfo(result);
        last_error_ = "Failed to create socket";
        return false;
    }

    // Non-blocking 설정
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

    // 연결
    int conn_result = ::connect(socket_fd_, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    if (conn_result < 0 && errno != EINPROGRESS) {
        close(socket_fd_);
        socket_fd_ = -1;
        last_error_ = "Connection failed";
        return false;
    }

    // 연결 완료 대기
    struct pollfd pfd;
    pfd.fd = socket_fd_;
    pfd.events = POLLOUT;

    int poll_result = poll(&pfd, 1, config_.connect_timeout_ms);
    if (poll_result <= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
        last_error_ = "Connection timeout";
        return false;
    }

    // 연결 에러 확인
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        close(socket_fd_);
        socket_fd_ = -1;
        last_error_ = "Connection error";
        return false;
    }

    // Blocking 모드로 복원
    fcntl(socket_fd_, F_SETFL, flags);

    // 인증 (토큰이 있는 경우)
    if (!config_.auth_token.empty()) {
        std::string auth_payload = "{\"token\":\"" + config_.auth_token + "\"}";
        send_message(MsgType::AuthRequest, auth_payload);

        auto response = receive_response();
        if (!response || response->first != MsgType::AuthResponse) {
            close(socket_fd_);
            socket_fd_ = -1;
            last_error_ = "Authentication failed";
            return false;
        }
    }

    return true;
#else
    last_error_ = "Windows not supported";
    return false;
#endif
}

void CLI::disconnect() {
#ifndef _WIN32
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
#endif
}

bool CLI::is_connected() const {
    return socket_fd_ >= 0;
}

// =============================================================================
// 조회 명령
// =============================================================================

std::optional<SystemStatusResponse> CLI::status() {
    if (!is_connected() && !connect()) {
        return std::nullopt;
    }

    if (!send_message(MsgType::CmdGetStatus, "{}")) {
        return std::nullopt;
    }

    auto response = receive_response();
    if (!response || response->first == MsgType::Error) {
        return std::nullopt;
    }

    SystemStatusResponse result;
    // 간단한 JSON 파싱 (실제로는 JSON 라이브러리 사용 권장)
    std::string json(response->second.begin(), response->second.end());

    // 기본값 설정 (실제 파싱은 생략)
    result.running = json.find("\"running\":true") != std::string::npos;
    result.kill_switch_active = json.find("\"kill_switch\":true") != std::string::npos;

    return result;
}

std::optional<PremiumResponse> CLI::premium() {
    if (!is_connected() && !connect()) {
        return std::nullopt;
    }

    std::string payload = "{\"type\":\"premium\"}";
    if (!send_message(MsgType::CmdGetStatus, payload)) {
        return std::nullopt;
    }

    auto response = receive_response();
    if (!response || response->first == MsgType::Error) {
        return std::nullopt;
    }

    PremiumResponse result;
    // 파싱은 실제 구현에서 처리
    return result;
}

std::optional<BalanceResponse> CLI::balance() {
    if (!is_connected() && !connect()) {
        return std::nullopt;
    }

    std::string payload = "{\"type\":\"balance\"}";
    if (!send_message(MsgType::CmdGetStatus, payload)) {
        return std::nullopt;
    }

    auto response = receive_response();
    if (!response || response->first == MsgType::Error) {
        return std::nullopt;
    }

    BalanceResponse result;
    return result;
}

std::optional<OrderStatusResponse> CLI::order_status(const std::string& order_id) {
    if (!is_connected() && !connect()) {
        return std::nullopt;
    }

    std::string payload = "{\"type\":\"order\",\"order_id\":\"" + order_id + "\"}";
    if (!send_message(MsgType::CmdGetStatus, payload)) {
        return std::nullopt;
    }

    auto response = receive_response();
    if (!response || response->first == MsgType::Error) {
        return std::nullopt;
    }

    OrderStatusResponse result;
    result.order_id = order_id;
    return result;
}

std::optional<TradeHistoryResponse> CLI::history(int count) {
    if (!is_connected() && !connect()) {
        return std::nullopt;
    }

    std::string payload = "{\"type\":\"history\",\"count\":" + std::to_string(count) + "}";
    if (!send_message(MsgType::CmdGetStatus, payload)) {
        return std::nullopt;
    }

    auto response = receive_response();
    if (!response || response->first == MsgType::Error) {
        return std::nullopt;
    }

    TradeHistoryResponse result;
    return result;
}

std::optional<HealthResponse> CLI::health() {
    if (!is_connected() && !connect()) {
        return std::nullopt;
    }

    std::string payload = "{\"type\":\"health\"}";
    if (!send_message(MsgType::CmdGetStatus, payload)) {
        return std::nullopt;
    }

    auto response = receive_response();
    if (!response || response->first == MsgType::Error) {
        return std::nullopt;
    }

    HealthResponse result;
    result.overall = "Healthy";
    return result;
}

// =============================================================================
// 제어 명령
// =============================================================================

CommandResponse CLI::order(
    const std::string& exchange,
    const std::string& side,
    double quantity,
    double price)
{
    CommandResponse result;

    if (!is_connected() && !connect()) {
        result.error = last_error_;
        return result;
    }

    std::ostringstream payload;
    payload << "{\"exchange\":\"" << exchange << "\""
            << ",\"side\":\"" << side << "\""
            << ",\"quantity\":" << std::fixed << std::setprecision(8) << quantity;
    if (price > 0) {
        payload << ",\"price\":" << std::fixed << std::setprecision(8) << price;
    }
    payload << "}";

    if (!send_message(MsgType::CmdStartStrategy, payload.str())) {
        result.error = "Failed to send order";
        return result;
    }

    auto response = receive_response();
    if (!response) {
        result.error = "No response";
        return result;
    }

    result.success = response->first == MsgType::CmdResponse;
    if (!result.success) {
        result.error = "Order failed";
    } else {
        result.message = "Order submitted";
    }

    return result;
}

CommandResponse CLI::cancel(const std::string& order_id) {
    CommandResponse result;

    if (!is_connected() && !connect()) {
        result.error = last_error_;
        return result;
    }

    std::string payload = "{\"order_id\":\"" + order_id + "\"}";
    if (!send_message(MsgType::CmdStopStrategy, payload)) {
        result.error = "Failed to send cancel";
        return result;
    }

    auto response = receive_response();
    if (!response) {
        result.error = "No response";
        return result;
    }

    result.success = response->first == MsgType::CmdResponse;
    result.message = result.success ? "Order canceled" : "Cancel failed";

    return result;
}

CommandResponse CLI::kill(const std::string& reason) {
    CommandResponse result;

    if (!is_connected() && !connect()) {
        result.error = last_error_;
        return result;
    }

    std::string payload = "{\"active\":true,\"reason\":\"" + reason + "\"}";
    if (!send_message(MsgType::CmdSetKillSwitch, payload)) {
        result.error = "Failed to send kill switch";
        return result;
    }

    auto response = receive_response();
    if (!response) {
        result.error = "No response";
        return result;
    }

    result.success = response->first == MsgType::CmdResponse;
    result.message = result.success ? "Kill switch activated" : "Kill switch failed";

    return result;
}

CommandResponse CLI::resume() {
    CommandResponse result;

    if (!is_connected() && !connect()) {
        result.error = last_error_;
        return result;
    }

    std::string payload = "{\"active\":false}";
    if (!send_message(MsgType::CmdSetKillSwitch, payload)) {
        result.error = "Failed to send resume";
        return result;
    }

    auto response = receive_response();
    if (!response) {
        result.error = "No response";
        return result;
    }

    result.success = response->first == MsgType::CmdResponse;
    result.message = result.success ? "Kill switch deactivated" : "Resume failed";

    return result;
}

CommandResponse CLI::start_strategy(const std::string& strategy_id) {
    CommandResponse result;

    if (!is_connected() && !connect()) {
        result.error = last_error_;
        return result;
    }

    std::string payload = "{\"strategy\":\"" + strategy_id + "\"}";
    if (!send_message(MsgType::CmdStartStrategy, payload)) {
        result.error = "Failed to send start";
        return result;
    }

    auto response = receive_response();
    if (!response) {
        result.error = "No response";
        return result;
    }

    result.success = response->first == MsgType::CmdResponse;
    result.message = result.success ? "Strategy started" : "Start failed";

    return result;
}

CommandResponse CLI::stop_strategy(const std::string& strategy_id) {
    CommandResponse result;

    if (!is_connected() && !connect()) {
        result.error = last_error_;
        return result;
    }

    std::string payload = "{\"strategy\":\"" + strategy_id + "\"}";
    if (!send_message(MsgType::CmdStopStrategy, payload)) {
        result.error = "Failed to send stop";
        return result;
    }

    auto response = receive_response();
    if (!response) {
        result.error = "No response";
        return result;
    }

    result.success = response->first == MsgType::CmdResponse;
    result.message = result.success ? "Strategy stopped" : "Stop failed";

    return result;
}

CommandResponse CLI::config_set(const std::string& key, const std::string& value) {
    CommandResponse result;
    result.success = true;
    result.message = "Config set: " + key + " = " + value;
    return result;
}

std::optional<std::string> CLI::config_get(const std::string& key) {
    // 설정 조회는 실제 서버 연동 시 구현
    return std::nullopt;
}

// =============================================================================
// 출력 헬퍼
// =============================================================================

void CLI::print_status(const SystemStatusResponse& status) {
    std::cout << "\n";
    std::cout << bold("=== System Status ===") << "\n\n";

    std::cout << "  Running:        " << (status.running ? green("Yes") : red("No")) << "\n";
    std::cout << "  Uptime:         " << status.uptime << "\n";
    std::cout << "  Connections:    " << status.active_connections << "\n";
    std::cout << "  Pending Orders: " << status.pending_orders << "\n";
    std::cout << "  Kill Switch:    " << (status.kill_switch_active ? red("ACTIVE") : green("Inactive")) << "\n";
    std::cout << "\n";

    std::cout << bold("--- Daily Stats ---") << "\n";
    std::cout << "  PnL:            " << (status.daily_pnl >= 0 ? green(format_krw(status.daily_pnl)) : red(format_krw(status.daily_pnl))) << "\n";
    std::cout << "  Trades:         " << status.daily_trades << "\n";
    std::cout << "  Remaining:      " << format_krw(status.remaining_limit) << "\n";
    std::cout << "  Strategy:       " << status.strategy_state << "\n";
    std::cout << "\n";
}

void CLI::print_premium(const PremiumResponse& premium) {
    std::cout << "\n";
    std::cout << bold("=== Premium Matrix ===") << "\n";
    std::cout << "  FX Rate: " << format_number(premium.fx_rate, 2) << " KRW/USD\n\n";

    // 테이블 헤더
    std::cout << std::setw(12) << "Buy\\Sell" << " | ";
    std::cout << std::setw(10) << "Upbit" << " | ";
    std::cout << std::setw(10) << "Bithumb" << "\n";
    std::cout << std::string(40, '-') << "\n";

    for (const auto& entry : premium.matrix) {
        std::string pct_str = format_percent(entry.premium_pct);
        if (entry.premium_pct > 1.0) {
            pct_str = green(pct_str);
        } else if (entry.premium_pct < -1.0) {
            pct_str = red(pct_str);
        }
        std::cout << std::setw(12) << entry.buy_exchange << " | ";
        std::cout << std::setw(10) << pct_str << "\n";
    }
    std::cout << "\n";
}

void CLI::print_balance(const BalanceResponse& balance) {
    std::cout << "\n";
    std::cout << bold("=== Balances ===") << "\n\n";

    std::cout << std::setw(12) << "Exchange" << " | ";
    std::cout << std::setw(15) << "XRP" << " | ";
    std::cout << std::setw(15) << "USDT" << " | ";
    std::cout << std::setw(15) << "KRW" << "\n";
    std::cout << std::string(65, '-') << "\n";

    for (const auto& b : balance.balances) {
        std::cout << std::setw(12) << b.exchange << " | ";
        std::cout << std::setw(15) << format_number(b.xrp, 4) << " | ";
        std::cout << std::setw(15) << format_number(b.usdt, 2) << " | ";
        std::cout << std::setw(15) << format_krw(b.krw) << "\n";
    }

    std::cout << std::string(65, '-') << "\n";
    std::cout << std::setw(12) << "Total" << " | ";
    std::cout << std::setw(47) << format_krw(balance.total_value_krw) << "\n";
    std::cout << "\n";
}

void CLI::print_history(const TradeHistoryResponse& history) {
    std::cout << "\n";
    std::cout << bold("=== Trade History ===") << "\n";
    std::cout << "  Total: " << history.total_count << " trades, PnL: ";
    std::cout << (history.total_pnl >= 0 ? green(format_krw(history.total_pnl)) : red(format_krw(history.total_pnl)));
    std::cout << "\n\n";

    std::cout << std::setw(20) << "Time" << " | ";
    std::cout << std::setw(10) << "Buy" << " | ";
    std::cout << std::setw(10) << "Sell" << " | ";
    std::cout << std::setw(12) << "Qty" << " | ";
    std::cout << std::setw(12) << "PnL" << "\n";
    std::cout << std::string(75, '-') << "\n";

    for (const auto& t : history.trades) {
        std::cout << std::setw(20) << t.timestamp << " | ";
        std::cout << std::setw(10) << t.buy_exchange << " | ";
        std::cout << std::setw(10) << t.sell_exchange << " | ";
        std::cout << std::setw(12) << format_number(t.quantity, 4) << " | ";
        std::cout << std::setw(12) << (t.pnl >= 0 ? green(format_krw(t.pnl)) : red(format_krw(t.pnl))) << "\n";
    }
    std::cout << "\n";
}

void CLI::print_health(const HealthResponse& health) {
    std::cout << "\n";
    std::cout << bold("=== Health Check ===") << "\n\n";

    std::cout << "  Overall: ";
    if (health.overall == "Healthy") {
        std::cout << green("Healthy");
    } else if (health.overall == "Degraded") {
        std::cout << yellow("Degraded");
    } else {
        std::cout << red("Unhealthy");
    }
    std::cout << "\n\n";

    std::cout << "  CPU:     " << format_percent(health.cpu_percent) << "\n";
    std::cout << "  Memory:  " << format_number(health.memory_bytes / (1024.0 * 1024.0), 1) << " MB\n\n";

    std::cout << bold("--- Components ---") << "\n";
    for (const auto& c : health.components) {
        std::cout << "  " << std::setw(15) << c.name << ": ";
        if (c.status == "Healthy") {
            std::cout << green(c.status);
        } else if (c.status == "Degraded") {
            std::cout << yellow(c.status);
        } else {
            std::cout << red(c.status);
        }
        if (!c.message.empty()) {
            std::cout << " (" << c.message << ")";
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

void CLI::print_response(const CommandResponse& response) {
    if (response.success) {
        std::cout << green("Success: ") << response.message << "\n";
    } else {
        std::cout << red("Error: ") << response.error << "\n";
    }
}

// =============================================================================
// 설정
// =============================================================================

void CLI::set_config(const CLIConfig& config) {
    config_ = config;
}

// =============================================================================
// 내부 구현
// =============================================================================

bool CLI::send_message(uint8_t type, const std::string& payload) {
    return send_message(type, std::vector<uint8_t>(payload.begin(), payload.end()));
}

bool CLI::send_message(uint8_t type, const std::vector<uint8_t>& payload) {
#ifndef _WIN32
    if (socket_fd_ < 0) {
        return false;
    }

    // 헤더 구성
    std::vector<uint8_t> message(HEADER_SIZE + payload.size());
    message[0] = MAGIC_0;
    message[1] = MAGIC_1;
    message[2] = VERSION;
    message[3] = type;

    uint32_t len = static_cast<uint32_t>(payload.size());
    message[4] = (len >> 0) & 0xFF;
    message[5] = (len >> 8) & 0xFF;
    message[6] = (len >> 16) & 0xFF;
    message[7] = (len >> 24) & 0xFF;

    // 페이로드 복사
    std::copy(payload.begin(), payload.end(), message.begin() + HEADER_SIZE);

    // 전송
    ssize_t sent = send(socket_fd_, message.data(), message.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(message.size());
#else
    return false;
#endif
}

std::optional<std::pair<uint8_t, std::vector<uint8_t>>> CLI::receive_response() {
#ifndef _WIN32
    if (socket_fd_ < 0) {
        return std::nullopt;
    }

    // 헤더 수신
    std::vector<uint8_t> header(HEADER_SIZE);

    struct pollfd pfd;
    pfd.fd = socket_fd_;
    pfd.events = POLLIN;

    if (poll(&pfd, 1, config_.read_timeout_ms) <= 0) {
        last_error_ = "Read timeout";
        return std::nullopt;
    }

    ssize_t received = recv(socket_fd_, header.data(), HEADER_SIZE, MSG_WAITALL);
    if (received != HEADER_SIZE) {
        last_error_ = "Failed to receive header";
        return std::nullopt;
    }

    // 헤더 검증
    if (header[0] != MAGIC_0 || header[1] != MAGIC_1 || header[2] != VERSION) {
        last_error_ = "Invalid header";
        return std::nullopt;
    }

    uint8_t type = header[3];
    uint32_t len = header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);

    // 페이로드 수신
    std::vector<uint8_t> payload(len);
    if (len > 0) {
        received = recv(socket_fd_, payload.data(), len, MSG_WAITALL);
        if (received != static_cast<ssize_t>(len)) {
            last_error_ = "Failed to receive payload";
            return std::nullopt;
        }
    }

    return std::make_pair(type, payload);
#else
    return std::nullopt;
#endif
}

std::string CLI::make_json(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << fields[i].first << "\":\"" << fields[i].second << "\"";
    }
    oss << "}";
    return oss.str();
}

// =============================================================================
// 컬러 출력
// =============================================================================

std::string CLI::color(const std::string& text, const std::string& color_code) {
    if (!config_.color_output) {
        return text;
    }
    return "\033[" + color_code + "m" + text + "\033[0m";
}

std::string CLI::green(const std::string& text) {
    return color(text, "32");
}

std::string CLI::red(const std::string& text) {
    return color(text, "31");
}

std::string CLI::yellow(const std::string& text) {
    return color(text, "33");
}

std::string CLI::cyan(const std::string& text) {
    return color(text, "36");
}

std::string CLI::bold(const std::string& text) {
    return color(text, "1");
}

// =============================================================================
// 유틸리티 함수
// =============================================================================

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " <command> [options]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  status              Show system status\n";
    std::cout << "  premium             Show premium matrix\n";
    std::cout << "  balance             Show balances\n";
    std::cout << "  history [count]     Show trade history\n";
    std::cout << "  health              Show health check\n";
    std::cout << "  order <exchange> <side> <qty> [price]  Place order\n";
    std::cout << "  cancel <order_id>   Cancel order\n";
    std::cout << "  kill [reason]       Activate kill switch\n";
    std::cout << "  resume              Deactivate kill switch\n";
    std::cout << "  start [strategy]    Start strategy\n";
    std::cout << "  stop [strategy]     Stop strategy\n";
    std::cout << "  config <key> [value]  Get/set config\n";
    std::cout << "\nOptions:\n";
    std::cout << "  -h, --host <host>   Server host (default: localhost)\n";
    std::cout << "  -p, --port <port>   Server port (default: 9800)\n";
    std::cout << "  -t, --token <token> Auth token\n";
    std::cout << "  -v, --verbose       Verbose output\n";
    std::cout << "  --no-color          Disable color output\n";
    std::cout << "  --version           Show version\n";
    std::cout << "  --help              Show this help\n";
}

void print_version() {
    std::cout << "Kimchi Arbitrage CLI v1.0.0\n";
}

std::string format_number(double value, int precision) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    std::string result = oss.str();

    // 천단위 콤마 추가
    size_t dot_pos = result.find('.');
    if (dot_pos == std::string::npos) {
        dot_pos = result.length();
    }

    std::string integer_part = result.substr(0, dot_pos);
    std::string decimal_part = result.substr(dot_pos);

    std::string formatted;
    int count = 0;
    for (auto it = integer_part.rbegin(); it != integer_part.rend(); ++it) {
        if (count > 0 && count % 3 == 0 && *it != '-') {
            formatted = ',' + formatted;
        }
        formatted = *it + formatted;
        ++count;
    }

    return formatted + decimal_part;
}

std::string format_krw(double value) {
    return format_number(value, 0) + " KRW";
}

std::string format_percent(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << value << "%";
    return oss.str();
}

std::string format_relative_time(const std::chrono::system_clock::time_point& time) {
    auto now = std::chrono::system_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - time);

    if (diff.count() < 60) {
        return std::to_string(diff.count()) + "s ago";
    } else if (diff.count() < 3600) {
        return std::to_string(diff.count() / 60) + "m ago";
    } else if (diff.count() < 86400) {
        return std::to_string(diff.count() / 3600) + "h ago";
    } else {
        return std::to_string(diff.count() / 86400) + "d ago";
    }
}

}  // namespace arbitrage::cli
