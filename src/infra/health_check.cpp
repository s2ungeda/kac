#include "arbitrage/infra/health_check.hpp"
#include "arbitrage/infra/event_bus.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef __linux__
#include <dirent.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#endif

namespace arbitrage {

// =============================================================================
// 싱글톤
// =============================================================================
HealthChecker& HealthChecker::instance() {
    static HealthChecker instance;
    return instance;
}

// =============================================================================
// 생성자/소멸자
// =============================================================================
HealthChecker::HealthChecker()
    : start_time_(std::chrono::steady_clock::now())
    , last_cpu_time_(std::chrono::steady_clock::now())
    , last_proc_ticks_(0)
{
}

HealthChecker::HealthChecker(const HealthCheckerConfig& config)
    : config_(config)
    , start_time_(std::chrono::steady_clock::now())
    , last_cpu_time_(std::chrono::steady_clock::now())
    , last_proc_ticks_(0)
{
}

HealthChecker::~HealthChecker() {
    stop();
}

// =============================================================================
// 체크 함수 등록
// =============================================================================
void HealthChecker::register_check(const std::string& name, CheckFunc check) {
    std::lock_guard<std::mutex> lock(checks_mutex_);
    checks_[name] = std::move(check);
}

void HealthChecker::unregister_check(const std::string& name) {
    std::lock_guard<std::mutex> lock(checks_mutex_);
    checks_.erase(name);
}

size_t HealthChecker::check_count() const {
    std::lock_guard<std::mutex> lock(checks_mutex_);
    return checks_.size();
}

// =============================================================================
// 상태 조회
// =============================================================================
SystemHealth HealthChecker::check_all() {
    SystemHealth health;
    health.timestamp = std::chrono::system_clock::now();

    // 체크 함수 복사 (락 최소화)
    std::unordered_map<std::string, CheckFunc> checks_copy;
    {
        std::lock_guard<std::mutex> lock(checks_mutex_);
        checks_copy = checks_;
    }

    // 각 컴포넌트 체크
    for (const auto& [name, check] : checks_copy) {
        try {
            auto component = check();
            health.components.push_back(component);

            switch (component.status) {
                case HealthStatus::Healthy:
                    ++health.healthy_count;
                    break;
                case HealthStatus::Degraded:
                    ++health.degraded_count;
                    break;
                case HealthStatus::Unhealthy:
                    ++health.unhealthy_count;
                    break;
            }
        } catch (const std::exception& e) {
            ComponentHealth failed;
            failed.name = name;
            failed.status = HealthStatus::Unhealthy;
            failed.message = std::string("Check exception: ") + e.what();
            failed.last_check = std::chrono::system_clock::now();
            health.components.push_back(failed);
            ++health.unhealthy_count;
        }
    }

    // 리소스 사용량
    if (config_.enable_resource_monitoring) {
        health.resources.cpu_percent = measure_cpu_usage();
        measure_memory_usage(health.resources);
        health.resources.open_fd_count = count_open_fds();
        health.resources.uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time_);

        // 리소스 기반 상태 체크
        if (health.resources.cpu_percent > config_.cpu_warning_threshold) {
            ComponentHealth cpu_warning;
            cpu_warning.name = "CPU";
            cpu_warning.status = HealthStatus::Degraded;
            cpu_warning.message = "High CPU usage: " +
                std::to_string(static_cast<int>(health.resources.cpu_percent)) + "%";
            cpu_warning.last_check = std::chrono::system_clock::now();
            health.components.push_back(cpu_warning);
            ++health.degraded_count;
        }

        if (health.resources.memory_percent > config_.memory_warning_threshold) {
            ComponentHealth mem_warning;
            mem_warning.name = "Memory";
            mem_warning.status = HealthStatus::Degraded;
            mem_warning.message = "High memory usage: " +
                std::to_string(static_cast<int>(health.resources.memory_percent)) + "%";
            mem_warning.last_check = std::chrono::system_clock::now();
            health.components.push_back(mem_warning);
            ++health.degraded_count;
        }
    }

    // 전체 상태 결정
    if (health.unhealthy_count > 0) {
        health.overall = HealthStatus::Unhealthy;
    } else if (health.degraded_count > 0) {
        health.overall = HealthStatus::Degraded;
    } else {
        health.overall = HealthStatus::Healthy;
    }

    // 통계
    total_checks_.fetch_add(1, std::memory_order_relaxed);

    // 캐시 업데이트
    {
        std::lock_guard<std::mutex> lock(health_mutex_);
        last_health_ = health;

        // 히스토리 추가
        history_.push_back(health);
        if (history_.size() > config_.history_size) {
            history_.erase(history_.begin());
        }
    }

    // 알림
    notify_alerts(health);

    return health;
}

ComponentHealth HealthChecker::check_component(const std::string& name) {
    CheckFunc check;
    {
        std::lock_guard<std::mutex> lock(checks_mutex_);
        auto it = checks_.find(name);
        if (it == checks_.end()) {
            ComponentHealth not_found;
            not_found.name = name;
            not_found.status = HealthStatus::Unhealthy;
            not_found.message = "Component not registered";
            not_found.last_check = std::chrono::system_clock::now();
            return not_found;
        }
        check = it->second;
    }

    try {
        return check();
    } catch (const std::exception& e) {
        ComponentHealth failed;
        failed.name = name;
        failed.status = HealthStatus::Unhealthy;
        failed.message = std::string("Check exception: ") + e.what();
        failed.last_check = std::chrono::system_clock::now();
        return failed;
    }
}

SystemHealth HealthChecker::last_health() const {
    std::lock_guard<std::mutex> lock(health_mutex_);
    return last_health_;
}

ResourceUsage HealthChecker::get_resource_usage() const {
    ResourceUsage usage;

    // CPU 측정은 시간 경과 필요하므로 캐시 반환
    {
        std::lock_guard<std::mutex> lock(health_mutex_);
        usage = last_health_.resources;
    }

    return usage;
}

// =============================================================================
// 주기적 체크
// =============================================================================
void HealthChecker::start_periodic_check() {
    start_periodic_check(config_.check_interval);
}

void HealthChecker::start_periodic_check(std::chrono::seconds interval) {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;  // 이미 실행 중
    }

    config_.check_interval = interval;
    worker_ = std::thread(&HealthChecker::worker_thread, this);
}

void HealthChecker::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;  // 이미 중지됨
    }

    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }
}

void HealthChecker::worker_thread() {
    while (running_.load(std::memory_order_acquire)) {
        check_all();

        // 인터벌 대기 (중간에 stop 가능)
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, config_.check_interval, [this]() {
            return !running_.load(std::memory_order_acquire);
        });
    }
}

// =============================================================================
// 콜백 설정
// =============================================================================
void HealthChecker::on_unhealthy(AlertCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    unhealthy_callbacks_.push_back(std::move(callback));
}

void HealthChecker::on_degraded(AlertCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    degraded_callbacks_.push_back(std::move(callback));
}

void HealthChecker::on_status_change(SystemAlertCallback callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    status_change_callbacks_.push_back(std::move(callback));
}

void HealthChecker::set_event_bus(std::shared_ptr<EventBus> bus) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    event_bus_ = bus;
}

// =============================================================================
// 설정
// =============================================================================
void HealthChecker::set_config(const HealthCheckerConfig& config) {
    std::lock_guard<std::mutex> lock(health_mutex_);
    config_ = config;
}

HealthCheckerConfig HealthChecker::config() const {
    std::lock_guard<std::mutex> lock(health_mutex_);
    return config_;
}

// =============================================================================
// 히스토리
// =============================================================================
std::vector<SystemHealth> HealthChecker::get_history() const {
    std::lock_guard<std::mutex> lock(health_mutex_);
    return history_;
}

// =============================================================================
// 리소스 측정
// =============================================================================
double HealthChecker::measure_cpu_usage() {
#ifdef __linux__
    try {
        // /proc/self/stat에서 프로세스 CPU 시간 읽기
        std::ifstream proc_stat("/proc/self/stat");
        if (!proc_stat.is_open()) {
            return 0.0;
        }

        std::string proc_line;
        if (!std::getline(proc_stat, proc_line)) {
            return 0.0;
        }

        // PID (comm) state ppid pgrp session tty_nr tpgid flags
        // minflt cminflt majflt cmajflt utime stime cutime cstime ...
        // Note: comm can contain spaces and parentheses, so we need to find the last ')'
        size_t comm_end = proc_line.rfind(')');
        if (comm_end == std::string::npos || comm_end + 2 >= proc_line.size()) {
            return 0.0;
        }

        // Parse from after the comm field
        std::istringstream proc_iss(proc_line.substr(comm_end + 2));

        // Skip fields 3-13 (state to cmajflt) - that's 11 fields
        std::string tmp;
        for (int i = 0; i < 11; ++i) {
            if (!(proc_iss >> tmp)) {
                return 0.0;
            }
        }

        // Read utime (field 14) and stime (field 15)
        uint64_t utime = 0, stime = 0;
        if (!(proc_iss >> utime >> stime)) {
            return 0.0;
        }

        uint64_t proc_time = utime + stime;

        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_cpu_time_).count();

        double cpu_percent = 0.0;

        if (elapsed_ms > 0 && last_proc_ticks_ > 0) {
            uint64_t proc_diff = proc_time - last_proc_ticks_;
            long ticks_per_sec = sysconf(_SC_CLK_TCK);
            if (ticks_per_sec > 0) {
                // Convert ticks to percentage
                cpu_percent = (static_cast<double>(proc_diff) * 1000.0) /
                              (static_cast<double>(elapsed_ms) * ticks_per_sec) * 100.0;
                cpu_percent = std::min(cpu_percent, 100.0);
                cpu_percent = std::max(cpu_percent, 0.0);
            }
        }

        last_cpu_time_ = now;
        last_proc_ticks_ = proc_time;

        return cpu_percent;
    } catch (...) {
        return 0.0;
    }
#else
    return 0.0;
#endif
}

void HealthChecker::measure_memory_usage(ResourceUsage& usage) {
#ifdef __linux__
    try {
        // /proc/self/status에서 메모리 사용량 읽기
        std::ifstream status_file("/proc/self/status");
        if (!status_file.is_open()) {
            return;
        }

        std::string line;
        while (std::getline(status_file, line)) {
            if (line.size() > 6 && line.substr(0, 6) == "VmRSS:") {
                // VmRSS: 실제 메모리 사용량 (kB)
                std::istringstream iss(line.substr(6));
                size_t kb = 0;
                iss >> kb;
                usage.memory_used_bytes = kb * 1024;
                break;
            }
        }

        // 시스템 총 메모리
        struct sysinfo info;
        memset(&info, 0, sizeof(info));
        if (sysinfo(&info) == 0) {
            usage.memory_total_bytes = static_cast<size_t>(info.totalram) *
                                       static_cast<size_t>(info.mem_unit);
            if (usage.memory_total_bytes > 0) {
                usage.memory_percent = static_cast<double>(usage.memory_used_bytes) /
                                       static_cast<double>(usage.memory_total_bytes) * 100.0;
            }
        }
    } catch (...) {
        // 무시
    }
#endif
}

size_t HealthChecker::count_open_fds() {
#ifdef __linux__
    try {
        size_t count = 0;
        DIR* dir = opendir("/proc/self/fd");
        if (dir != nullptr) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                ++count;
            }
            closedir(dir);
            // . 과 .. 제외
            count = count > 2 ? count - 2 : 0;
        }
        return count;
    } catch (...) {
        return 0;
    }
#else
    return 0;
#endif
}

// =============================================================================
// 알림
// =============================================================================
void HealthChecker::notify_alerts(const SystemHealth& health) {
    std::vector<AlertCallback> unhealthy_cbs, degraded_cbs;
    std::vector<SystemAlertCallback> status_cbs;
    std::shared_ptr<EventBus> bus;

    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        unhealthy_cbs = unhealthy_callbacks_;
        degraded_cbs = degraded_callbacks_;
        status_cbs = status_change_callbacks_;
        bus = event_bus_.lock();
    }

    // Unhealthy 컴포넌트 알림
    for (const auto& component : health.components) {
        if (component.is_unhealthy()) {
            for (const auto& cb : unhealthy_cbs) {
                try {
                    cb(component);
                } catch (const std::exception& e) {
                    // Unhealthy 콜백 에러 (처리 계속)
                }
            }
        } else if (component.is_degraded()) {
            for (const auto& cb : degraded_cbs) {
                try {
                    cb(component);
                } catch (const std::exception& e) {
                    // Degraded 콜백 에러 (처리 계속)
                }
            }
        }
    }

    // 시스템 상태 변경 알림
    for (const auto& cb : status_cbs) {
        try {
            cb(health);
        } catch (const std::exception& e) {
            // 상태 변경 콜백 에러 (처리 계속)
        }
    }

    // EventBus로 KillSwitch 이벤트 발행 (Unhealthy인 경우)
    if (bus && health.overall == HealthStatus::Unhealthy) {
        events::KillSwitchActivated event;
        event.reason = "Health check failed";
        event.manual = false;
        bus->publish(event);
    }
}

}  // namespace arbitrage
