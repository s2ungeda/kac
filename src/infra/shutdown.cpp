#include "arbitrage/infra/shutdown.hpp"
#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/infra/events.hpp"

#include <algorithm>
#include <csignal>
#include <iostream>
#include <thread>

namespace arbitrage {

// =============================================================================
// 싱글톤
// =============================================================================
namespace { ShutdownManager* g_set_shutdown_manager_override = nullptr; }
ShutdownManager& ShutdownManager::instance() {
    if (g_set_shutdown_manager_override) return *g_set_shutdown_manager_override;
    static ShutdownManager instance;
    return instance;
}
void set_shutdown_manager(ShutdownManager* p) { g_set_shutdown_manager_override = p; }

// =============================================================================
// 생성자/소멸자
// =============================================================================
ShutdownManager::ShutdownManager() = default;

ShutdownManager::~ShutdownManager() {
    if (handlers_installed_) {
        uninstall_signal_handlers();
    }
}

// =============================================================================
// 시그널 핸들러
// =============================================================================
void ShutdownManager::install_signal_handlers() {
    SpinLockGuard lock(mutex_);

    if (handlers_installed_) {
        return;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifndef _WIN32
    std::signal(SIGHUP, signal_handler);
#endif

    handlers_installed_ = true;
}

void ShutdownManager::uninstall_signal_handlers() {
    SpinLockGuard lock(mutex_);

    if (!handlers_installed_) {
        return;
    }

    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
#ifndef _WIN32
    std::signal(SIGHUP, SIG_DFL);
#endif

    handlers_installed_ = false;
}

void ShutdownManager::signal_handler(int signum) {
    // 시그널 핸들러에서는 async-signal-safe 함수만 호출해야 하므로
    // 플래그만 설정하고 실제 종료는 다른 스레드에서 처리
    auto& manager = instance();

    bool expected = false;
    if (manager.shutting_down_.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        // 첫 번째 시그널: 정상 종료 시작
        manager.shutdown_reason_ = std::string("Signal ") + signal_name(signum);
        manager.phase_.store(ShutdownPhase::Initiated, std::memory_order_release);

        // 별도 스레드에서 종료 처리
        std::thread([&manager]() {
            manager.do_shutdown();
        }).detach();
    } else {
        // 두 번째 시그널: 강제 종료
        std::cerr << "\n[SHUTDOWN] Received second signal, forcing exit...\n";
        std::_Exit(128 + signum);
    }
}

const char* ShutdownManager::signal_name(int signum) {
    switch (signum) {
        case SIGINT:  return "SIGINT";
        case SIGTERM: return "SIGTERM";
#ifndef _WIN32
        case SIGHUP:  return "SIGHUP";
        case SIGQUIT: return "SIGQUIT";
#endif
        default: return "UNKNOWN";
    }
}

// =============================================================================
// 컴포넌트 등록
// =============================================================================
void ShutdownManager::register_component(const std::string& name,
                                         ShutdownCallback callback) {
    register_component(name, std::move(callback), ShutdownPriority::Default);
}

void ShutdownManager::register_component(const std::string& name,
                                         ShutdownCallback callback,
                                         int priority,
                                         std::chrono::milliseconds timeout) {
    SpinLockGuard lock(mutex_);

    // 중복 체크
    auto it = std::find_if(components_.begin(), components_.end(),
        [&name](const ShutdownComponent& c) { return c.name == name; });

    if (it != components_.end()) {
        // 기존 컴포넌트 업데이트
        it->callback = std::move(callback);
        it->priority = priority;
        it->timeout = timeout;
    } else {
        // 새 컴포넌트 추가
        components_.push_back({
            name,
            std::move(callback),
            priority,
            timeout,
            false,
            std::chrono::milliseconds(0)
        });
    }
}

void ShutdownManager::unregister_component(const std::string& name) {
    SpinLockGuard lock(mutex_);

    components_.erase(
        std::remove_if(components_.begin(), components_.end(),
            [&name](const ShutdownComponent& c) { return c.name == name; }),
        components_.end());
}

size_t ShutdownManager::component_count() const {
    SpinLockGuard lock(mutex_);
    return components_.size();
}

// =============================================================================
// 종료 제어
// =============================================================================
void ShutdownManager::initiate_shutdown(const std::string& reason) {
    bool expected = false;
    if (!shutting_down_.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        // 이미 종료 중
        return;
    }

    shutdown_reason_ = reason.empty() ? "Requested" : reason;
    phase_.store(ShutdownPhase::Initiated, std::memory_order_release);

    // 별도 스레드에서 종료 처리
    std::thread([this]() {
        do_shutdown();
    }).detach();
}

ShutdownResult ShutdownManager::wait_for_shutdown(std::chrono::milliseconds timeout) {
    bool completed = SpinWait::until_for([this] {
        return shutdown_wakeup_.load(std::memory_order_acquire) ||
               phase_.load(std::memory_order_acquire) == ShutdownPhase::Completed ||
               phase_.load(std::memory_order_acquire) == ShutdownPhase::Timeout;
    }, timeout);
    shutdown_wakeup_.store(false, std::memory_order_release);

    if (!completed) {
        result_.phase = ShutdownPhase::Timeout;
    }

    return result_;
}

void ShutdownManager::force_shutdown() {
    shutting_down_.store(true, std::memory_order_release);
    phase_.store(ShutdownPhase::Timeout, std::memory_order_release);
    shutdown_wakeup_.store(true, std::memory_order_release);
}

bool ShutdownManager::cancel_shutdown() {
    // Initiated 단계에서만 취소 가능
    ShutdownPhase expected = ShutdownPhase::Initiated;
    if (phase_.compare_exchange_strong(expected, ShutdownPhase::Running,
            std::memory_order_acq_rel)) {
        shutting_down_.store(false, std::memory_order_release);
        shutdown_reason_.clear();
        return true;
    }
    return false;
}

// =============================================================================
// 상태 조회
// =============================================================================
std::string ShutdownManager::shutdown_reason() const {
    SpinLockGuard lock(mutex_);
    return shutdown_reason_;
}

int ShutdownManager::progress_percent() const {
    SpinLockGuard lock(mutex_);

    if (components_.empty()) {
        return is_shutting_down() ? 100 : 0;
    }

    size_t completed = 0;
    for (const auto& c : components_) {
        if (c.completed) {
            ++completed;
        }
    }

    return static_cast<int>(completed * 100 / components_.size());
}

// =============================================================================
// 콜백 설정
// =============================================================================
void ShutdownManager::set_progress_callback(ProgressCallback callback) {
    SpinLockGuard lock(mutex_);
    progress_callback_ = std::move(callback);
}

void ShutdownManager::set_event_bus(std::shared_ptr<EventBus> bus) {
    SpinLockGuard lock(mutex_);
    event_bus_ = bus;
}

// =============================================================================
// 실제 종료 처리
// =============================================================================
void ShutdownManager::do_shutdown() {
    auto start_time = std::chrono::steady_clock::now();

    phase_.store(ShutdownPhase::Stopping, std::memory_order_release);

    // 컴포넌트 목록 복사 및 정렬
    std::vector<ShutdownComponent> components_copy;
    {
        SpinLockGuard lock(mutex_);
        components_copy = components_;
    }

    // 우선순위로 정렬 (낮은 순)
    std::sort(components_copy.begin(), components_copy.end(),
        [](const ShutdownComponent& a, const ShutdownComponent& b) {
            return a.priority < b.priority;
        });

    // SystemShutdown 이벤트 발행
    if (auto bus = event_bus_.lock()) {
        events::SystemShutdown event;
        event.reason = shutdown_reason_;
        event.graceful = true;
        bus->publish(event);
    }

    std::cerr << "\n[SHUTDOWN] Starting graceful shutdown...\n";
    std::cerr << "[SHUTDOWN] Reason: " << shutdown_reason_ << "\n";
    std::cerr << "[SHUTDOWN] Components to stop: " << components_copy.size() << "\n";

    // 각 컴포넌트 종료
    for (auto& component : components_copy) {
        std::cerr << "[SHUTDOWN] Stopping: " << component.name << " (priority="
                  << component.priority << ")...\n";

        bool success = stop_component(component);

        // 원본 컴포넌트 상태 업데이트
        {
            SpinLockGuard lock(mutex_);
            auto it = std::find_if(components_.begin(), components_.end(),
                [&component](const ShutdownComponent& c) {
                    return c.name == component.name;
                });
            if (it != components_.end()) {
                it->completed = component.completed;
                it->elapsed = component.elapsed;
            }

            // 진행 콜백 호출 (inline progress calc to avoid recursive lock)
            if (progress_callback_) {
                size_t done = 0;
                for (const auto& c : components_) {
                    if (c.completed) ++done;
                }
                int progress = components_.empty() ? 100
                    : static_cast<int>(done * 100 / components_.size());
                progress_callback_(component.name, progress);
            }
        }

        if (success) {
            result_.completed_components.push_back(component.name);
            std::cerr << "[SHUTDOWN] Stopped: " << component.name
                      << " (" << component.elapsed.count() << "ms)\n";
        } else if (component.elapsed >= component.timeout) {
            result_.timeout_components.push_back(component.name);
            std::cerr << "[SHUTDOWN] TIMEOUT: " << component.name << "\n";
        } else {
            result_.failed_components.push_back(component.name);
            std::cerr << "[SHUTDOWN] FAILED: " << component.name << "\n";
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    result_.total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);

    if (result_.timeout_components.empty() && result_.failed_components.empty()) {
        result_.phase = ShutdownPhase::Completed;
        std::cerr << "[SHUTDOWN] Graceful shutdown completed ("
                  << result_.total_elapsed.count() << "ms)\n";
    } else {
        result_.phase = ShutdownPhase::Timeout;
        std::cerr << "[SHUTDOWN] Shutdown completed with issues ("
                  << result_.total_elapsed.count() << "ms)\n";
    }

    phase_.store(result_.phase, std::memory_order_release);
    shutdown_wakeup_.store(true, std::memory_order_release);
}

bool ShutdownManager::stop_component(ShutdownComponent& component) {
    auto start = std::chrono::steady_clock::now();

    try {
        // 타임아웃 스레드
        std::atomic<bool> done{false};
        std::atomic<bool> timeout_flag{false};

        std::thread timeout_thread([&]() {
            auto deadline = std::chrono::steady_clock::now() + component.timeout;
            while (!done.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    timeout_flag.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        // 콜백 실행
        if (component.callback) {
            component.callback();
        }

        done.store(true, std::memory_order_release);
        timeout_thread.join();

        auto end = std::chrono::steady_clock::now();
        component.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start);

        if (timeout_flag.load(std::memory_order_acquire)) {
            return false;
        }

        component.completed = true;
        return true;
    } catch (const std::exception& e) {
        auto end = std::chrono::steady_clock::now();
        component.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start);

        std::cerr << "[SHUTDOWN] Exception in " << component.name
                  << ": " << e.what() << "\n";
        return false;
    } catch (...) {
        std::cerr << "[SHUTDOWN] Unknown exception in " << component.name << "\n";
        return false;
    }
}

}  // namespace arbitrage
