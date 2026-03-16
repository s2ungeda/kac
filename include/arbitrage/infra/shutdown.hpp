#pragma once

/**
 * Shutdown Manager (TASK_21)
 *
 * 안전한 시스템 종료 처리
 * - SIGINT/SIGTERM 시그널 처리
 * - 컴포넌트 순차 종료
 * - 타임아웃 처리
 * - EventBus 연동
 */

#include "arbitrage/common/error.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace arbitrage {

// Forward declarations
class EventBus;

// =============================================================================
// 종료 단계
// =============================================================================
enum class ShutdownPhase {
    Running,      // 정상 실행 중
    Initiated,    // 종료 시작됨
    Stopping,     // 컴포넌트 종료 중
    Completed,    // 종료 완료
    Timeout       // 타임아웃으로 강제 종료
};

/**
 * 종료 단계 이름
 */
inline const char* shutdown_phase_name(ShutdownPhase phase) {
    switch (phase) {
        case ShutdownPhase::Running:   return "Running";
        case ShutdownPhase::Initiated: return "Initiated";
        case ShutdownPhase::Stopping:  return "Stopping";
        case ShutdownPhase::Completed: return "Completed";
        case ShutdownPhase::Timeout:   return "Timeout";
        default: return "Unknown";
    }
}

// =============================================================================
// 종료 우선순위 (낮은 숫자 = 먼저 종료)
// =============================================================================
namespace ShutdownPriority {
    constexpr int Network    = 100;   // 네트워크 연결 (WebSocket)
    constexpr int Order      = 200;   // 주문 관련
    constexpr int Transfer   = 300;   // 송금 관련
    constexpr int Strategy   = 400;   // 전략 엔진
    constexpr int Storage    = 500;   // 저장소 (DB, 파일)
    constexpr int Logging    = 900;   // 로깅 (마지막)
    constexpr int Default    = 500;   // 기본값
}

// =============================================================================
// 컴포넌트 정보
// =============================================================================
struct ShutdownComponent {
    std::string name;
    std::function<void()> callback;
    int priority{ShutdownPriority::Default};
    std::chrono::milliseconds timeout{std::chrono::seconds(5)};
    bool completed{false};
    std::chrono::milliseconds elapsed{0};
};

// =============================================================================
// 종료 결과
// =============================================================================
struct ShutdownResult {
    ShutdownPhase phase{ShutdownPhase::Running};
    std::chrono::milliseconds total_elapsed{0};
    std::vector<std::string> completed_components;
    std::vector<std::string> timeout_components;
    std::vector<std::string> failed_components;
    std::string error_message;

    bool success() const {
        return phase == ShutdownPhase::Completed &&
               timeout_components.empty() &&
               failed_components.empty();
    }
};

// =============================================================================
// ShutdownManager
// =============================================================================
class ShutdownManager {
public:
    using ShutdownCallback = std::function<void()>;
    using ProgressCallback = std::function<void(const std::string& component, int progress_pct)>;

    /**
     * 싱글톤 인스턴스
     */
    static ShutdownManager& instance();

    ShutdownManager();
    ~ShutdownManager();

    ShutdownManager(const ShutdownManager&) = delete;
    ShutdownManager& operator=(const ShutdownManager&) = delete;

    // =========================================================================
    // 시그널 핸들러
    // =========================================================================

    /**
     * SIGINT/SIGTERM 핸들러 등록
     */
    void install_signal_handlers();

    /**
     * 시그널 핸들러 해제
     */
    void uninstall_signal_handlers();

    // =========================================================================
    // 컴포넌트 등록
    // =========================================================================

    /**
     * 컴포넌트 등록 (기본 우선순위)
     * @param name 컴포넌트 이름 (로깅용)
     * @param callback 종료 시 호출할 콜백
     */
    void register_component(const std::string& name, ShutdownCallback callback);

    /**
     * 컴포넌트 등록 (우선순위 지정)
     * @param name 컴포넌트 이름
     * @param callback 종료 콜백
     * @param priority 우선순위 (낮을수록 먼저 종료)
     * @param timeout 타임아웃 (기본 5초)
     */
    void register_component(const std::string& name,
                           ShutdownCallback callback,
                           int priority,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5));

    /**
     * 컴포넌트 등록 해제
     */
    void unregister_component(const std::string& name);

    /**
     * 등록된 컴포넌트 수
     */
    size_t component_count() const;

    // =========================================================================
    // 종료 제어
    // =========================================================================

    /**
     * 종료 시작 (비동기)
     * @param reason 종료 사유
     */
    void initiate_shutdown(const std::string& reason = "");

    /**
     * 종료 대기
     * @param timeout 전체 타임아웃
     * @return 종료 결과
     */
    ShutdownResult wait_for_shutdown(
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    /**
     * 강제 종료
     */
    void force_shutdown();

    /**
     * 종료 취소 (아직 시작되지 않은 경우만)
     * @return 취소 성공 여부
     */
    bool cancel_shutdown();

    // =========================================================================
    // 상태 조회
    // =========================================================================

    /**
     * 종료 중인지 확인
     */
    bool is_shutting_down() const {
        return shutting_down_.load(std::memory_order_acquire);
    }

    /**
     * 현재 종료 단계
     */
    ShutdownPhase phase() const {
        return phase_.load(std::memory_order_acquire);
    }

    /**
     * 종료 사유
     */
    std::string shutdown_reason() const;

    /**
     * 현재 종료 진행률 (0-100)
     */
    int progress_percent() const;

    // =========================================================================
    // 콜백 설정
    // =========================================================================

    /**
     * 진행 콜백 설정
     */
    void set_progress_callback(ProgressCallback callback);

    /**
     * EventBus 연결 (SystemShutdown 이벤트 발행)
     */
    void set_event_bus(std::shared_ptr<EventBus> bus);

    // =========================================================================
    // 유틸리티
    // =========================================================================

    /**
     * 시그널 번호 이름
     */
    static const char* signal_name(int signum);

private:
    /**
     * 시그널 핸들러 (static)
     */
    static void signal_handler(int signum);

    /**
     * 실제 종료 처리
     */
    void do_shutdown();

    /**
     * 컴포넌트 종료 (타임아웃 포함)
     */
    bool stop_component(ShutdownComponent& component);

private:
    // 상태
    std::atomic<bool> shutting_down_{false};
    std::atomic<ShutdownPhase> phase_{ShutdownPhase::Running};

    // 컴포넌트
    mutable std::mutex mutex_;
    std::vector<ShutdownComponent> components_;

    // 종료 대기
    std::condition_variable shutdown_cv_;
    std::string shutdown_reason_;

    // 콜백
    ProgressCallback progress_callback_;
    std::weak_ptr<EventBus> event_bus_;

    // 결과
    ShutdownResult result_;

    // 시그널 핸들러 원본 (복원용)
    bool handlers_installed_{false};
};

// =============================================================================
// 글로벌 접근자
// =============================================================================

/**
 * ShutdownManager 싱글톤 접근
 */
inline ShutdownManager& shutdown_manager() {
    return ShutdownManager::instance();
}

// =============================================================================
// RAII 종료 가드
// =============================================================================
class ShutdownGuard {
public:
    ShutdownGuard(const std::string& name, ShutdownManager::ShutdownCallback callback)
        : name_(name)
    {
        shutdown_manager().register_component(name, std::move(callback));
    }

    ShutdownGuard(const std::string& name,
                  ShutdownManager::ShutdownCallback callback,
                  int priority)
        : name_(name)
    {
        shutdown_manager().register_component(name, std::move(callback), priority);
    }

    ~ShutdownGuard() {
        shutdown_manager().unregister_component(name_);
    }

    ShutdownGuard(const ShutdownGuard&) = delete;
    ShutdownGuard& operator=(const ShutdownGuard&) = delete;

private:
    std::string name_;
};

}  // namespace arbitrage
