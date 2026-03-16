#pragma once

/**
 * Alert System (TASK_24)
 *
 * 텔레그램/Discord 알림 발송
 * - 레벨별 알림 (Info, Warning, Error, Critical)
 * - Rate Limiting
 * - 포맷팅 지원
 * - EventBus 연동
 */

#include "arbitrage/common/error.hpp"
#include "arbitrage/common/http_client.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace arbitrage {

// Forward declarations
class EventBus;

// =============================================================================
// 알림 레벨
// =============================================================================

enum class AlertLevel {
    Info,       // 정보성 알림
    Warning,    // 경고
    Error,      // 오류
    Critical    // 심각한 오류 (즉시 대응 필요)
};

/**
 * 알림 레벨 이름
 */
inline const char* alert_level_name(AlertLevel level) {
    switch (level) {
        case AlertLevel::Info:     return "INFO";
        case AlertLevel::Warning:  return "WARNING";
        case AlertLevel::Error:    return "ERROR";
        case AlertLevel::Critical: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

/**
 * 알림 레벨 이모지
 */
inline const char* alert_level_emoji(AlertLevel level) {
    switch (level) {
        case AlertLevel::Info:     return "ℹ️";
        case AlertLevel::Warning:  return "⚠️";
        case AlertLevel::Error:    return "❌";
        case AlertLevel::Critical: return "🚨";
        default: return "❓";
    }
}

// =============================================================================
// 알림 구조체
// =============================================================================

struct Alert {
    AlertLevel level{AlertLevel::Info};
    std::string title;
    std::string message;
    std::chrono::system_clock::time_point timestamp;
    std::string source;           // 알림 발생 소스 (컴포넌트명)
    std::unordered_map<std::string, std::string> metadata;  // 추가 데이터

    Alert() : timestamp(std::chrono::system_clock::now()) {}

    Alert(AlertLevel lvl, const std::string& t, const std::string& msg)
        : level(lvl)
        , title(t)
        , message(msg)
        , timestamp(std::chrono::system_clock::now())
    {}

    /**
     * 텔레그램용 포맷
     */
    std::string format_telegram() const;

    /**
     * Discord용 포맷 (JSON)
     */
    std::string format_discord() const;

    /**
     * 간단한 텍스트 포맷
     */
    std::string format_text() const;
};

// =============================================================================
// 알림 채널 설정
// =============================================================================

/**
 * 텔레그램 설정
 */
struct TelegramConfig {
    bool enabled{false};
    std::string bot_token;
    std::string chat_id;
    AlertLevel min_level{AlertLevel::Warning};  // 최소 알림 레벨
    bool parse_mode_html{true};                 // HTML 파싱 모드
};

/**
 * Discord 설정
 */
struct DiscordConfig {
    bool enabled{false};
    std::string webhook_url;
    AlertLevel min_level{AlertLevel::Warning};
    std::string username{"Kimchi Bot"};         // 봇 이름
    std::string avatar_url;                     // 봇 아바타 URL
};

/**
 * 슬랙 설정 (선택적)
 */
struct SlackConfig {
    bool enabled{false};
    std::string webhook_url;
    AlertLevel min_level{AlertLevel::Warning};
    std::string channel;
    std::string username{"Kimchi Bot"};
};

/**
 * Rate Limit 설정
 */
struct AlertRateLimitConfig {
    size_t max_per_minute{30};          // 분당 최대 알림 수
    size_t max_per_hour{200};           // 시간당 최대 알림 수
    std::chrono::seconds cooldown{5};   // 동일 알림 쿨다운
    bool aggregate_similar{true};       // 유사 알림 집계
};

/**
 * 전체 알림 설정
 */
struct AlertConfig {
    TelegramConfig telegram;
    DiscordConfig discord;
    SlackConfig slack;
    AlertRateLimitConfig rate_limit;
    bool async_send{true};              // 비동기 전송
    size_t queue_size{1000};            // 알림 큐 크기
    std::chrono::seconds retry_delay{5}; // 재시도 간격
    int max_retries{3};                 // 최대 재시도 횟수
};

// =============================================================================
// 알림 서비스
// =============================================================================

class AlertService {
public:
    /**
     * 싱글톤 인스턴스
     */
    static AlertService& instance();

    AlertService();
    explicit AlertService(const AlertConfig& config);
    ~AlertService();

    AlertService(const AlertService&) = delete;
    AlertService& operator=(const AlertService&) = delete;

    // =========================================================================
    // 서비스 제어
    // =========================================================================

    /**
     * 서비스 시작 (비동기 워커)
     */
    void start();

    /**
     * 서비스 중지
     */
    void stop();

    /**
     * 실행 중인지 확인
     */
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // =========================================================================
    // 알림 전송
    // =========================================================================

    /**
     * 알림 전송 (비동기)
     */
    std::future<Result<void>> send(const Alert& alert);

    /**
     * 알림 전송 (동기)
     */
    Result<void> send_sync(const Alert& alert);

    /**
     * 편의 메서드 - Info
     */
    void info(const std::string& title, const std::string& message);

    /**
     * 편의 메서드 - Warning
     */
    void warning(const std::string& title, const std::string& message);

    /**
     * 편의 메서드 - Error
     */
    void error(const std::string& title, const std::string& message);

    /**
     * 편의 메서드 - Critical
     */
    void critical(const std::string& title, const std::string& message);

    /**
     * 소스 지정 알림
     */
    void send_from(const std::string& source, AlertLevel level,
                   const std::string& title, const std::string& message);

    // =========================================================================
    // 설정
    // =========================================================================

    /**
     * 설정 업데이트
     */
    void set_config(const AlertConfig& config);

    /**
     * 현재 설정
     */
    AlertConfig config() const;

    /**
     * HTTP 클라이언트 설정
     */
    void set_http_client(std::unique_ptr<HttpClient> client);

    /**
     * EventBus 연결
     */
    void set_event_bus(std::shared_ptr<EventBus> bus);

    // =========================================================================
    // 통계
    // =========================================================================

    struct Stats {
        std::atomic<uint64_t> total_sent{0};
        std::atomic<uint64_t> total_failed{0};
        std::atomic<uint64_t> total_rate_limited{0};
        std::atomic<uint64_t> telegram_sent{0};
        std::atomic<uint64_t> discord_sent{0};
        std::atomic<uint64_t> slack_sent{0};
        std::chrono::steady_clock::time_point started_at;
    };

    const Stats& stats() const { return stats_; }

    /**
     * 큐에 대기 중인 알림 수
     */
    size_t pending_count() const;

private:
    // =========================================================================
    // 내부 구현
    // =========================================================================

    /**
     * 워커 스레드
     */
    void worker_thread();

    /**
     * Rate limit 체크
     */
    bool check_rate_limit(const Alert& alert);

    /**
     * 텔레그램 전송
     */
    Result<void> send_telegram(const Alert& alert);

    /**
     * Discord 전송
     */
    Result<void> send_discord(const Alert& alert);

    /**
     * Slack 전송
     */
    Result<void> send_slack(const Alert& alert);

    /**
     * 알림 처리
     */
    Result<void> process_alert(const Alert& alert);

private:
    AlertConfig config_;

    // HTTP 클라이언트
    std::unique_ptr<HttpClient> http_client_;

    // 워커 스레드
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    // 알림 큐
    mutable std::mutex queue_mutex_;
    std::deque<std::pair<Alert, std::promise<Result<void>>>> queue_;

    // Rate limiting
    mutable std::mutex rate_limit_mutex_;
    std::deque<std::chrono::steady_clock::time_point> recent_alerts_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_alert_time_;

    // EventBus
    std::weak_ptr<EventBus> event_bus_;

    // 통계
    Stats stats_;
};

// =============================================================================
// 글로벌 접근자
// =============================================================================

/**
 * AlertService 싱글톤 접근
 */
inline AlertService& alert_service() {
    return AlertService::instance();
}

// =============================================================================
// 편의 함수
// =============================================================================

/**
 * 간단한 알림 전송
 */
inline void send_alert(AlertLevel level, const std::string& title,
                       const std::string& message)
{
    alert_service().send(Alert{level, title, message});
}

inline void alert_info(const std::string& title, const std::string& message) {
    alert_service().info(title, message);
}

inline void alert_warning(const std::string& title, const std::string& message) {
    alert_service().warning(title, message);
}

inline void alert_error(const std::string& title, const std::string& message) {
    alert_service().error(title, message);
}

inline void alert_critical(const std::string& title, const std::string& message) {
    alert_service().critical(title, message);
}

}  // namespace arbitrage
