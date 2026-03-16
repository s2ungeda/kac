#pragma once

/**
 * Health Check System (TASK_22)
 *
 * 시스템 상태 점검 및 모니터링
 * - 컴포넌트 상태 체크
 * - CPU/메모리 모니터링
 * - 주기적 자동 체크
 * - 이상 시 알림 콜백
 */

#include "arbitrage/infra/events.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
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
// 상태 정의
// =============================================================================

/**
 * 컴포넌트 상태
 */
enum class HealthStatus {
    Healthy,    // 정상
    Degraded,   // 성능 저하
    Unhealthy   // 비정상
};

/**
 * 상태 이름
 */
inline const char* health_status_name(HealthStatus status) {
    switch (status) {
        case HealthStatus::Healthy:   return "Healthy";
        case HealthStatus::Degraded:  return "Degraded";
        case HealthStatus::Unhealthy: return "Unhealthy";
        default: return "Unknown";
    }
}

// =============================================================================
// 상태 구조체
// =============================================================================

/**
 * 컴포넌트 상태
 */
struct ComponentHealth {
    std::string name;
    HealthStatus status{HealthStatus::Healthy};
    std::string message;
    std::chrono::system_clock::time_point last_check;
    std::chrono::milliseconds response_time{0};

    ComponentHealth() = default;
    ComponentHealth(const std::string& n, HealthStatus s, const std::string& msg = "")
        : name(n)
        , status(s)
        , message(msg)
        , last_check(std::chrono::system_clock::now())
    {}

    bool is_healthy() const { return status == HealthStatus::Healthy; }
    bool is_degraded() const { return status == HealthStatus::Degraded; }
    bool is_unhealthy() const { return status == HealthStatus::Unhealthy; }
};

/**
 * 시스템 리소스 정보
 */
struct ResourceUsage {
    double cpu_percent{0.0};        // CPU 사용률 (0-100)
    size_t memory_used_bytes{0};    // 사용 중인 메모리 (bytes)
    size_t memory_total_bytes{0};   // 전체 메모리 (bytes)
    double memory_percent{0.0};     // 메모리 사용률 (0-100)
    size_t open_fd_count{0};        // 열린 파일 디스크립터 수
    std::chrono::seconds uptime{0}; // 프로세스 실행 시간
};

/**
 * 전체 시스템 상태
 */
struct SystemHealth {
    HealthStatus overall{HealthStatus::Healthy};
    std::vector<ComponentHealth> components;
    ResourceUsage resources;
    std::chrono::system_clock::time_point timestamp;
    size_t healthy_count{0};
    size_t degraded_count{0};
    size_t unhealthy_count{0};

    SystemHealth() : timestamp(std::chrono::system_clock::now()) {}

    double health_ratio() const {
        size_t total = components.size();
        if (total == 0) return 1.0;
        return static_cast<double>(healthy_count) / total;
    }
};

// =============================================================================
// HealthChecker 설정
// =============================================================================

struct HealthCheckerConfig {
    std::chrono::seconds check_interval{30};    // 체크 주기
    std::chrono::milliseconds check_timeout{5000}; // 체크 타임아웃
    double cpu_warning_threshold{80.0};         // CPU 경고 임계값 (%)
    double memory_warning_threshold{80.0};      // 메모리 경고 임계값 (%)
    bool enable_resource_monitoring{true};      // 리소스 모니터링 활성화
    size_t history_size{100};                   // 상태 히스토리 크기
};

// =============================================================================
// HealthChecker
// =============================================================================

class HealthChecker {
public:
    using CheckFunc = std::function<ComponentHealth()>;
    using AlertCallback = std::function<void(const ComponentHealth&)>;
    using SystemAlertCallback = std::function<void(const SystemHealth&)>;

    /**
     * 싱글톤 인스턴스
     */
    static HealthChecker& instance();

    HealthChecker();
    explicit HealthChecker(const HealthCheckerConfig& config);
    ~HealthChecker();

    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;

    // =========================================================================
    // 체크 함수 등록
    // =========================================================================

    /**
     * 컴포넌트 체크 함수 등록
     * @param name 컴포넌트 이름
     * @param check 체크 함수 (ComponentHealth 반환)
     */
    void register_check(const std::string& name, CheckFunc check);

    /**
     * 컴포넌트 체크 함수 해제
     */
    void unregister_check(const std::string& name);

    /**
     * 등록된 체크 수
     */
    size_t check_count() const;

    // =========================================================================
    // 상태 조회
    // =========================================================================

    /**
     * 전체 시스템 상태 조회
     */
    SystemHealth check_all();

    /**
     * 특정 컴포넌트 상태 조회
     */
    ComponentHealth check_component(const std::string& name);

    /**
     * 마지막 시스템 상태 (캐시)
     */
    SystemHealth last_health() const;

    /**
     * 리소스 사용량 조회
     */
    ResourceUsage get_resource_usage() const;

    // =========================================================================
    // 주기적 체크
    // =========================================================================

    /**
     * 주기적 체크 시작
     */
    void start_periodic_check();
    void start_periodic_check(std::chrono::seconds interval);

    /**
     * 주기적 체크 중지
     */
    void stop();

    /**
     * 실행 중인지 확인
     */
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // =========================================================================
    // 콜백 설정
    // =========================================================================

    /**
     * Unhealthy 상태 알림 콜백
     */
    void on_unhealthy(AlertCallback callback);

    /**
     * Degraded 상태 알림 콜백
     */
    void on_degraded(AlertCallback callback);

    /**
     * 시스템 상태 변경 콜백
     */
    void on_status_change(SystemAlertCallback callback);

    /**
     * EventBus 연결
     */
    void set_event_bus(std::shared_ptr<EventBus> bus);

    // =========================================================================
    // 설정
    // =========================================================================

    /**
     * 설정 업데이트
     */
    void set_config(const HealthCheckerConfig& config);

    /**
     * 현재 설정
     */
    HealthCheckerConfig config() const;

    // =========================================================================
    // 통계
    // =========================================================================

    /**
     * 체크 횟수
     */
    uint64_t total_checks() const {
        return total_checks_.load(std::memory_order_relaxed);
    }

    /**
     * 상태 히스토리 (최근 N개)
     */
    std::vector<SystemHealth> get_history() const;

private:
    /**
     * 워커 스레드
     */
    void worker_thread();

    /**
     * CPU 사용량 측정
     */
    double measure_cpu_usage();

    /**
     * 메모리 사용량 측정
     */
    void measure_memory_usage(ResourceUsage& usage);

    /**
     * 파일 디스크립터 수 측정
     */
    size_t count_open_fds();

    /**
     * 알림 콜백 호출
     */
    void notify_alerts(const SystemHealth& health);

private:
    HealthCheckerConfig config_;

    // 체크 함수
    mutable std::mutex checks_mutex_;
    std::unordered_map<std::string, CheckFunc> checks_;

    // 워커 스레드
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    // 상태 캐시
    mutable std::mutex health_mutex_;
    SystemHealth last_health_;
    std::vector<SystemHealth> history_;

    // 콜백
    std::mutex callbacks_mutex_;
    std::vector<AlertCallback> unhealthy_callbacks_;
    std::vector<AlertCallback> degraded_callbacks_;
    std::vector<SystemAlertCallback> status_change_callbacks_;
    std::weak_ptr<EventBus> event_bus_;

    // 프로세스 시작 시간
    std::chrono::steady_clock::time_point start_time_;

    // CPU 측정용
    std::chrono::steady_clock::time_point last_cpu_time_;
    uint64_t last_proc_ticks_{0};

    // 통계
    std::atomic<uint64_t> total_checks_{0};
};

// =============================================================================
// 글로벌 접근자
// =============================================================================

/**
 * HealthChecker 싱글톤 접근
 */
inline HealthChecker& health_checker() {
    return HealthChecker::instance();
}

// =============================================================================
// RAII 체크 등록 가드
// =============================================================================

class HealthCheckGuard {
public:
    HealthCheckGuard(const std::string& name, HealthChecker::CheckFunc check)
        : name_(name)
    {
        health_checker().register_check(name, std::move(check));
    }

    ~HealthCheckGuard() {
        health_checker().unregister_check(name_);
    }

    HealthCheckGuard(const HealthCheckGuard&) = delete;
    HealthCheckGuard& operator=(const HealthCheckGuard&) = delete;

private:
    std::string name_;
};

// =============================================================================
// 편의 함수
// =============================================================================

/**
 * 간단한 상태 체크 함수 생성
 */
inline HealthChecker::CheckFunc make_simple_check(
    const std::string& name,
    std::function<bool()> is_healthy,
    const std::string& healthy_msg = "OK",
    const std::string& unhealthy_msg = "Failed")
{
    return [=]() -> ComponentHealth {
        auto start = std::chrono::steady_clock::now();
        bool healthy = false;
        try {
            healthy = is_healthy();
        } catch (...) {
            healthy = false;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        ComponentHealth result;
        result.name = name;
        result.status = healthy ? HealthStatus::Healthy : HealthStatus::Unhealthy;
        result.message = healthy ? healthy_msg : unhealthy_msg;
        result.last_check = std::chrono::system_clock::now();
        result.response_time = elapsed;
        return result;
    };
}

/**
 * 조건부 상태 체크 함수 생성 (Healthy/Degraded/Unhealthy)
 */
inline HealthChecker::CheckFunc make_conditional_check(
    const std::string& name,
    std::function<HealthStatus()> get_status,
    std::function<std::string()> get_message = nullptr)
{
    return [=]() -> ComponentHealth {
        auto start = std::chrono::steady_clock::now();
        HealthStatus status = HealthStatus::Unhealthy;
        std::string message;

        try {
            status = get_status();
            if (get_message) {
                message = get_message();
            }
        } catch (const std::exception& e) {
            status = HealthStatus::Unhealthy;
            message = std::string("Exception: ") + e.what();
        } catch (...) {
            status = HealthStatus::Unhealthy;
            message = "Unknown exception";
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        ComponentHealth result;
        result.name = name;
        result.status = status;
        result.message = message;
        result.last_check = std::chrono::system_clock::now();
        result.response_time = elapsed;
        return result;
    };
}

}  // namespace arbitrage
