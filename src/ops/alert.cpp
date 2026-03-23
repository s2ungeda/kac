#include "arbitrage/ops/alert.hpp"
#include "arbitrage/infra/event_bus.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace arbitrage {

// =============================================================================
// Alert 포맷팅
// =============================================================================

std::string Alert::format_telegram() const {
    std::ostringstream ss;

    // 이모지와 레벨
    ss << alert_level_emoji(level) << " <b>" << alert_level_name(level) << "</b>\n";

    // 타이틀
    ss << "<b>" << title << "</b>\n\n";

    // 메시지
    ss << message;

    // 소스
    if (!source.empty()) {
        ss << "\n\n<i>Source: " << source << "</i>";
    }

    // 타임스탬프
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    ss << "\n<i>" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "</i>";

    return ss.str();
}

std::string Alert::format_discord() const {
    std::ostringstream ss;

    // Discord embed JSON
    ss << "{\"embeds\":[{";

    // 색상 (레벨별)
    int color = 0x808080;  // 기본 회색
    switch (level) {
        case AlertLevel::Info:     color = 0x3498db; break;  // 파랑
        case AlertLevel::Warning:  color = 0xf39c12; break;  // 주황
        case AlertLevel::Error:    color = 0xe74c3c; break;  // 빨강
        case AlertLevel::Critical: color = 0x9b59b6; break;  // 보라
    }
    ss << "\"color\":" << color << ",";

    // 타이틀
    ss << "\"title\":\"" << alert_level_emoji(level) << " " << title << "\",";

    // 설명
    ss << "\"description\":\"" << message << "\",";

    // 필드
    ss << "\"fields\":[";
    ss << "{\"name\":\"Level\",\"value\":\"" << alert_level_name(level) << "\",\"inline\":true}";
    if (!source.empty()) {
        ss << ",{\"name\":\"Source\",\"value\":\"" << source << "\",\"inline\":true}";
    }
    ss << "],";

    // 타임스탬프 (ISO 8601)
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    ss << "\"timestamp\":\"";
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    ss << "\"";

    ss << "}]}";

    return ss.str();
}

std::string Alert::format_text() const {
    std::ostringstream ss;

    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    ss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] ";
    ss << "[" << alert_level_name(level) << "] ";
    ss << title << ": " << message;

    if (!source.empty()) {
        ss << " (from: " << source << ")";
    }

    return ss.str();
}

// =============================================================================
// 싱글톤
// =============================================================================

AlertService& AlertService::instance() {
    static AlertService instance;
    return instance;
}

// =============================================================================
// 생성자/소멸자
// =============================================================================

AlertService::AlertService() {
    stats_.started_at = std::chrono::steady_clock::now();
}

AlertService::AlertService(const AlertConfig& config)
    : config_(config)
{
    stats_.started_at = std::chrono::steady_clock::now();
}

AlertService::~AlertService() {
    stop();
}

// =============================================================================
// 서비스 제어
// =============================================================================

void AlertService::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;  // 이미 실행 중
    }

    // HTTP 클라이언트 생성 (없으면)
    if (!http_client_) {
        http_client_ = create_http_client();
    }

    worker_ = std::thread(&AlertService::worker_thread, this);
}

void AlertService::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }
}

// =============================================================================
// 알림 전송
// =============================================================================

std::future<Result<void>> AlertService::send(const Alert& alert) {
    std::promise<Result<void>> promise;
    auto future = promise.get_future();

    // Rate limit 체크
    if (!check_rate_limit(alert)) {
        stats_.total_rate_limited.fetch_add(1, std::memory_order_relaxed);
        promise.set_value(Err<void>(ErrorCode::RateLimited, "Alert rate limited"));
        return future;
    }

    if (config_.async_send && running_.load(std::memory_order_acquire)) {
        // 비동기: 큐에 추가
        std::lock_guard<std::mutex> lock(queue_mutex_);

        if (queue_.size() >= config_.queue_size) {
            promise.set_value(Err<void>(ErrorCode::InternalError, "Alert queue full"));
            return future;
        }

        queue_.emplace_back(alert, std::move(promise));
        cv_.notify_one();
    } else {
        // 동기 전송
        auto result = process_alert(alert);
        promise.set_value(result);
    }

    return future;
}

Result<void> AlertService::send_sync(const Alert& alert) {
    if (!check_rate_limit(alert)) {
        stats_.total_rate_limited.fetch_add(1, std::memory_order_relaxed);
        return Err<void>(ErrorCode::RateLimited, "Alert rate limited");
    }

    return process_alert(alert);
}

void AlertService::info(const std::string& title, const std::string& message) {
    send(Alert{AlertLevel::Info, title, message});
}

void AlertService::warning(const std::string& title, const std::string& message) {
    send(Alert{AlertLevel::Warning, title, message});
}

void AlertService::error(const std::string& title, const std::string& message) {
    send(Alert{AlertLevel::Error, title, message});
}

void AlertService::critical(const std::string& title, const std::string& message) {
    send(Alert{AlertLevel::Critical, title, message});
}

void AlertService::send_from(const std::string& source, AlertLevel level,
                             const std::string& title, const std::string& message)
{
    Alert alert{level, title, message};
    alert.source = source;
    send(alert);
}

// =============================================================================
// 설정
// =============================================================================

void AlertService::set_config(const AlertConfig& config) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    config_ = config;
}

AlertConfig AlertService::config() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return config_;
}

void AlertService::set_http_client(std::unique_ptr<HttpClient> client) {
    http_client_ = std::move(client);
}

void AlertService::set_event_bus(std::shared_ptr<EventBus> bus) {
    event_bus_ = bus;
}

size_t AlertService::pending_count() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size();
}

// =============================================================================
// 내부 구현
// =============================================================================

void AlertService::worker_thread() {
    while (running_.load(std::memory_order_acquire)) {
        std::pair<Alert, std::promise<Result<void>>> item;
        bool has_item = false;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return !queue_.empty() || !running_.load(std::memory_order_acquire);
            });

            if (!queue_.empty()) {
                item = std::move(queue_.front());
                queue_.pop_front();
                has_item = true;
            }
        }

        if (has_item) {
            auto result = process_alert(item.first);
            item.second.set_value(result);
        }
    }

    // 남은 알림 처리
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!queue_.empty()) {
        auto& item = queue_.front();
        auto result = process_alert(item.first);
        item.second.set_value(result);
        queue_.pop_front();
    }
}

bool AlertService::check_rate_limit(const Alert& alert) {
    std::lock_guard<std::mutex> lock(rate_limit_mutex_);

    auto now = std::chrono::steady_clock::now();

    // 분당 제한 체크
    auto one_minute_ago = now - std::chrono::minutes(1);
    while (!recent_alerts_.empty() && recent_alerts_.front() < one_minute_ago) {
        recent_alerts_.pop_front();
    }

    if (recent_alerts_.size() >= config_.rate_limit.max_per_minute) {
        return false;
    }

    // 동일 알림 쿨다운 체크
    if (config_.rate_limit.aggregate_similar) {
        std::string key = alert.title + ":" + std::to_string(static_cast<int>(alert.level));
        auto it = last_alert_time_.find(key);
        if (it != last_alert_time_.end()) {
            if (now - it->second < config_.rate_limit.cooldown) {
                return false;
            }
        }
        last_alert_time_[key] = now;
    }

    recent_alerts_.push_back(now);
    return true;
}

Result<void> AlertService::send_telegram(const Alert& alert) {
    if (!config_.telegram.enabled) {
        return Ok();
    }

    if (static_cast<int>(alert.level) < static_cast<int>(config_.telegram.min_level)) {
        return Ok();  // 레벨 미달
    }

    if (!http_client_) {
        return Err<void>(ErrorCode::InvalidState, "HTTP client not initialized");
    }

    // Telegram Bot API URL
    std::string url = "https://api.telegram.org/bot" + config_.telegram.bot_token + "/sendMessage";

    // 요청 본문
    std::string body = "chat_id=" + config_.telegram.chat_id;
    body += "&text=" + alert.format_telegram();
    if (config_.telegram.parse_mode_html) {
        body += "&parse_mode=HTML";
    }

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    auto result = http_client_->post(url, body, headers);

    if (!result.has_value()) {
        return Err<void>(ErrorCode::NetworkError, "Telegram API error: " + result.error().message);
    }

    if (!result.value().is_success()) {
        return Err<void>(ErrorCode::ApiError,
            "Telegram API returned " + std::to_string(result.value().status_code));
    }

    stats_.telegram_sent.fetch_add(1, std::memory_order_relaxed);
    return Ok();
}

Result<void> AlertService::send_discord(const Alert& alert) {
    if (!config_.discord.enabled) {
        return Ok();
    }

    if (static_cast<int>(alert.level) < static_cast<int>(config_.discord.min_level)) {
        return Ok();
    }

    if (!http_client_) {
        return Err<void>(ErrorCode::InvalidState, "HTTP client not initialized");
    }

    std::string body = alert.format_discord();

    // username과 avatar 추가
    if (!config_.discord.username.empty() || !config_.discord.avatar_url.empty()) {
        // JSON 수정 (단순 삽입)
        size_t pos = body.find("{\"embeds\"");
        if (pos != std::string::npos) {
            std::string prefix = "{";
            if (!config_.discord.username.empty()) {
                prefix += "\"username\":\"" + config_.discord.username + "\",";
            }
            if (!config_.discord.avatar_url.empty()) {
                prefix += "\"avatar_url\":\"" + config_.discord.avatar_url + "\",";
            }
            body = prefix + "\"embeds\"" + body.substr(pos + 9);
        }
    }

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";

    auto result = http_client_->post(config_.discord.webhook_url, body, headers);

    if (!result.has_value()) {
        return Err<void>(ErrorCode::NetworkError, "Discord API error: " + result.error().message);
    }

    // Discord webhook은 204 No Content 반환
    if (result.value().status_code != 204 && !result.value().is_success()) {
        return Err<void>(ErrorCode::ApiError,
            "Discord API returned " + std::to_string(result.value().status_code));
    }

    stats_.discord_sent.fetch_add(1, std::memory_order_relaxed);
    return Ok();
}

Result<void> AlertService::send_slack(const Alert& alert) {
    if (!config_.slack.enabled) {
        return Ok();
    }

    if (static_cast<int>(alert.level) < static_cast<int>(config_.slack.min_level)) {
        return Ok();
    }

    if (!http_client_) {
        return Err<void>(ErrorCode::InvalidState, "HTTP client not initialized");
    }

    // Slack message format
    std::ostringstream ss;
    ss << "{";
    if (!config_.slack.channel.empty()) {
        ss << "\"channel\":\"" << config_.slack.channel << "\",";
    }
    if (!config_.slack.username.empty()) {
        ss << "\"username\":\"" << config_.slack.username << "\",";
    }

    // 텍스트
    ss << "\"text\":\"" << alert_level_emoji(alert.level) << " *"
       << alert.title << "*\\n" << alert.message << "\"";

    // 첨부 파일 (색상)
    ss << ",\"attachments\":[{\"color\":\"";
    switch (alert.level) {
        case AlertLevel::Info:     ss << "#3498db"; break;
        case AlertLevel::Warning:  ss << "#f39c12"; break;
        case AlertLevel::Error:    ss << "#e74c3c"; break;
        case AlertLevel::Critical: ss << "#9b59b6"; break;
    }
    ss << "\",\"text\":\"Level: " << alert_level_name(alert.level);
    if (!alert.source.empty()) {
        ss << " | Source: " << alert.source;
    }
    ss << "\"}]";

    ss << "}";

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";

    auto result = http_client_->post(config_.slack.webhook_url, ss.str(), headers);

    if (!result.has_value()) {
        return Err<void>(ErrorCode::NetworkError, "Slack API error: " + result.error().message);
    }

    if (!result.value().is_success()) {
        return Err<void>(ErrorCode::ApiError,
            "Slack API returned " + std::to_string(result.value().status_code));
    }

    stats_.slack_sent.fetch_add(1, std::memory_order_relaxed);
    return Ok();
}

Result<void> AlertService::process_alert(const Alert& alert) {
    // 모든 채널이 비활성화면 즉시 성공
    if (!config_.telegram.enabled && !config_.discord.enabled && !config_.slack.enabled) {
        return Ok();
    }

    bool any_sent = false;
    std::string last_error;

    // 각 채널로 전송 시도
    for (int retry = 0; retry <= config_.max_retries; ++retry) {
        if (retry > 0) {
            std::this_thread::sleep_for(config_.retry_delay);
        }

        // Telegram
        if (config_.telegram.enabled) {
            auto result = send_telegram(alert);
            if (result.has_value()) {
                any_sent = true;
            } else {
                last_error = result.error().message;
            }
        }

        // Discord
        if (config_.discord.enabled) {
            auto result = send_discord(alert);
            if (result.has_value()) {
                any_sent = true;
            } else {
                last_error = result.error().message;
            }
        }

        // Slack
        if (config_.slack.enabled) {
            auto result = send_slack(alert);
            if (result.has_value()) {
                any_sent = true;
            } else {
                last_error = result.error().message;
            }
        }

        if (any_sent) {
            break;
        }
    }

    if (any_sent) {
        stats_.total_sent.fetch_add(1, std::memory_order_relaxed);
        return Ok();
    } else {
        stats_.total_failed.fetch_add(1, std::memory_order_relaxed);

        // 모든 채널이 비활성화된 경우는 성공으로 처리
        if (!config_.telegram.enabled && !config_.discord.enabled && !config_.slack.enabled) {
            return Ok();
        }

        return Err<void>(ErrorCode::NetworkError, "Failed to send alert: " + last_error);
    }
}

}  // namespace arbitrage
