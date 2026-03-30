#include "arbitrage/infra/watchdog_child.hpp"

#include <algorithm>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#endif

namespace arbitrage {

// =============================================================================
// 알림 헬퍼
// =============================================================================

void ChildProcessManager::send_alert(const std::string& level, const std::string& message) {
    if (alert_func_) {
        alert_func_(level, message);
    }
}

// =============================================================================
// 자식 프로세스 등록/해제
// =============================================================================

void ChildProcessManager::add_child(const ChildProcessConfig& config) {
    std::lock_guard<std::mutex> lock(children_mutex_);
    ChildProcessInfo info;
    info.config = config;
    children_[config.name] = info;
}

void ChildProcessManager::remove_child(const std::string& name) {
    std::lock_guard<std::mutex> lock(children_mutex_);
    auto it = children_.find(name);
    if (it == children_.end()) return;

    if (it->second.is_running && it->second.pid > 0) {
        kill_child(it->second.pid);
    }
    children_.erase(it);
}

// =============================================================================
// 시작/중지
// =============================================================================

void ChildProcessManager::launch_all_children() {
    std::lock_guard<std::mutex> lock(children_mutex_);

    // start_order로 정렬
    std::vector<std::string> ordered_names;
    ordered_names.reserve(children_.size());
    for (auto& [name, _] : children_) {
        ordered_names.push_back(name);
    }
    std::sort(ordered_names.begin(), ordered_names.end(),
        [this](const std::string& a, const std::string& b) {
            return children_[a].config.start_order < children_[b].config.start_order;
        });

    int prev_order = -1;
    for (const auto& name : ordered_names) {
        auto& info = children_[name];

        // start_delay: 이전 순서 그룹과 다르면 대기
        if (prev_order >= 0 && info.config.start_order > prev_order
            && info.config.start_delay_ms > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(info.config.start_delay_ms));
        }
        prev_order = info.config.start_order;

        int pid = launch_child(info.config);
        if (pid > 0) {
            info.pid = pid;
            info.is_running = true;
            info.start_time = std::chrono::system_clock::now();
            info.last_restart_time = std::chrono::steady_clock::now();
            send_alert("info", "Child process started: " + name + " (pid=" + std::to_string(pid) + ")");
        } else {
            info.is_running = false;
            info.last_error = "Failed to launch";
            send_alert("error", "Failed to launch child: " + name);
            if (info.config.critical) {
                send_alert("critical", "Critical child " + name + " failed to start — aborting");
                return;
            }
        }
    }
}

void ChildProcessManager::stop_all_children() {
    std::lock_guard<std::mutex> lock(children_mutex_);

    // 역순 종료 (start_order 높은 것부터)
    std::vector<std::string> ordered_names;
    ordered_names.reserve(children_.size());
    for (auto& [name, _] : children_) {
        ordered_names.push_back(name);
    }
    std::sort(ordered_names.begin(), ordered_names.end(),
        [this](const std::string& a, const std::string& b) {
            return children_[a].config.start_order > children_[b].config.start_order;
        });

    for (const auto& name : ordered_names) {
        auto& info = children_[name];
        if (info.is_running && info.pid > 0) {
            kill_child(info.pid);
            info.is_running = false;
            info.pid = -1;
        }
    }
}

void ChildProcessManager::restart_child(const std::string& name, const std::string& reason) {
    std::lock_guard<std::mutex> lock(children_mutex_);
    auto it = children_.find(name);
    if (it == children_.end()) return;

    auto& info = it->second;

    // 기존 프로세스 종료
    if (info.is_running && info.pid > 0) {
        kill_child(info.pid);
        info.is_running = false;
    }

    // 재시작 가능 여부 확인
    if (!can_restart_child(info)) {
        info.last_error = "Max restarts exceeded";
        send_alert("critical", "Child " + name + " exceeded max restarts");
        if (info.config.critical) {
            send_alert("critical", "Critical child " + name + " permanently failed — shutting down");
            // 전체 시스템 종료 트리거
            if (shutdown_func_) {
                shutdown_func_();
            }
        }
        return;
    }

    // 재시작 딜레이
    if (info.config.restart_delay_ms > 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(info.config.restart_delay_ms));
    }

    int pid = launch_child(info.config);
    if (pid > 0) {
        info.pid = pid;
        info.is_running = true;
        info.restart_count++;
        info.start_time = std::chrono::system_clock::now();
        info.last_restart_time = std::chrono::steady_clock::now();
        send_alert("warning", "Child restarted: " + name + " (pid=" + std::to_string(pid)
                   + ", reason=" + reason + ", restarts=" + std::to_string(info.restart_count) + ")");
    } else {
        info.last_error = "Restart failed";
        send_alert("error", "Failed to restart child: " + name);
    }
}

// =============================================================================
// 모니터링
// =============================================================================

void ChildProcessManager::check_children(std::atomic<bool>& system_running,
                                          std::condition_variable& cv) {
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(children_mutex_);

    for (auto& [name, info] : children_) {
        if (!info.is_running || info.pid <= 0) continue;

        // 프로세스 살아있는지 확인
        int status = 0;
        int result = ::waitpid(info.pid, &status, WNOHANG);

        if (result == 0) {
            // 아직 실행 중
            continue;
        }

        if (result == info.pid) {
            // 프로세스 종료됨
            info.is_running = false;

            std::string reason;
            if (WIFEXITED(status)) {
                info.exit_code = WEXITSTATUS(status);
                reason = "exited with code " + std::to_string(info.exit_code);
            } else if (WIFSIGNALED(status)) {
                info.exit_code = -WTERMSIG(status);
                reason = "killed by signal " + std::to_string(WTERMSIG(status));
            } else {
                reason = "terminated (unknown)";
            }

            info.last_error = reason;
            send_alert("warning", "Child " + name + " " + reason);

            // 자동 재시작 (lock은 이미 잡고 있으므로 직접 실행)
            if (system_running.load(std::memory_order_acquire)) {
                if (can_restart_child(info)) {
                    if (info.config.restart_delay_ms > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(info.config.restart_delay_ms));
                    }
                    int new_pid = launch_child(info.config);
                    if (new_pid > 0) {
                        info.pid = new_pid;
                        info.is_running = true;
                        info.restart_count++;
                        info.start_time = std::chrono::system_clock::now();
                        info.last_restart_time = std::chrono::steady_clock::now();
                        send_alert("info", "Child auto-restarted: " + name
                                   + " (pid=" + std::to_string(new_pid)
                                   + ", restarts=" + std::to_string(info.restart_count) + ")");
                    } else {
                        info.last_error = "Auto-restart failed";
                        send_alert("error", "Auto-restart failed for child: " + name);
                        if (info.config.critical) {
                            send_alert("critical", "Critical child " + name + " cannot restart");
                            system_running.store(false, std::memory_order_release);
                            cv.notify_all();
                            return;
                        }
                    }
                } else {
                    send_alert("critical", "Child " + name + " exceeded max restarts ("
                               + std::to_string(info.restart_count) + ")");
                    if (info.config.critical) {
                        system_running.store(false, std::memory_order_release);
                        cv.notify_all();
                        return;
                    }
                }
            }
        } else if (result < 0 && errno == ECHILD) {
            // 프로세스가 존재하지 않음
            info.is_running = false;
            info.pid = -1;
        }
    }
#endif
}

// =============================================================================
// 조회
// =============================================================================

std::vector<ChildProcessInfo> ChildProcessManager::get_children_status() const {
    std::lock_guard<std::mutex> lock(children_mutex_);
    std::vector<ChildProcessInfo> result;
    result.reserve(children_.size());
    for (const auto& [_, info] : children_) {
        result.push_back(info);
    }
    return result;
}

// =============================================================================
// 기본 구성 생성
// =============================================================================

std::vector<ChildProcessConfig> ChildProcessManager::make_default_children(
    const std::string& bin_dir,
    const std::vector<std::string>& engine_args)
{
    std::vector<ChildProcessConfig> children;

    // 4 Feeders (start_order=0, 동시 시작)
    const char* feeders[] = {"upbit-feeder", "bithumb-feeder", "binance-feeder", "mexc-feeder"};
    for (const char* name : feeders) {
        ChildProcessConfig cfg;
        cfg.name = name;
        cfg.executable = bin_dir + "/" + name;
        cfg.restart_delay_ms = 2000;
        cfg.max_restarts = 10;
        cfg.critical = true;
        cfg.start_order = 0;
        cfg.start_delay_ms = 0;
        children.push_back(cfg);
    }

    // Engine (start_order=1, Feeder 시작 후 1초 대기)
    ChildProcessConfig engine_cfg;
    engine_cfg.name = "arb-engine";
    engine_cfg.executable = bin_dir + "/arbitrage";
    engine_cfg.arguments = {"--engine"};
    for (const auto& arg : engine_args) {
        engine_cfg.arguments.push_back(arg);
    }
    engine_cfg.restart_delay_ms = 3000;
    engine_cfg.max_restarts = 10;
    engine_cfg.critical = true;
    engine_cfg.start_order = 1;
    engine_cfg.start_delay_ms = 1000;
    children.push_back(engine_cfg);

    // TASK_48: Cold Path 프로세스 (start_order=2~3)

    // Order Manager (start_order=2)
    ChildProcessConfig om_cfg;
    om_cfg.name = "order-manager";
    om_cfg.executable = bin_dir + "/order-manager";
    om_cfg.restart_delay_ms = 2000;
    om_cfg.max_restarts = 10;
    om_cfg.critical = true;
    om_cfg.start_order = 2;
    om_cfg.start_delay_ms = 500;
    children.push_back(om_cfg);

    // Risk Manager (start_order=2, order-manager와 병렬)
    ChildProcessConfig rm_cfg;
    rm_cfg.name = "risk-manager";
    rm_cfg.executable = bin_dir + "/risk-manager";
    rm_cfg.restart_delay_ms = 2000;
    rm_cfg.max_restarts = 10;
    rm_cfg.critical = false;  // 리스크 매니저 없어도 엔진 실행 가능
    rm_cfg.start_order = 2;
    rm_cfg.start_delay_ms = 0;
    children.push_back(rm_cfg);

    // Monitor (start_order=3, 마지막)
    ChildProcessConfig mon_cfg;
    mon_cfg.name = "monitor";
    mon_cfg.executable = bin_dir + "/monitor";
    mon_cfg.restart_delay_ms = 2000;
    mon_cfg.max_restarts = 10;
    mon_cfg.critical = false;
    mon_cfg.start_order = 3;
    mon_cfg.start_delay_ms = 500;
    children.push_back(mon_cfg);

    return children;
}

// =============================================================================
// 프로세스 시작/종료 유틸리티
// =============================================================================

int ChildProcessManager::launch_child(const ChildProcessConfig& config) {
#ifndef _WIN32
    pid_t pid = fork();

    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        // 자식 프로세스
        if (!working_directory_.empty()) {
            (void)chdir(working_directory_.c_str());
        }

        std::vector<char*> args;
        args.push_back(const_cast<char*>(config.executable.c_str()));
        for (const auto& arg : config.arguments) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);

        execvp(config.executable.c_str(), args.data());
        _exit(127);  // exec 실패
    }

    return pid;
#else
    return -1;
#endif
}

void ChildProcessManager::kill_child(int pid, int timeout_ms) {
#ifndef _WIN32
    if (pid <= 0) return;

    // SIGTERM 전송
    if (::kill(pid, SIGTERM) < 0) {
        if (errno == ESRCH) return;  // 이미 종료됨
    }

    // 종료 대기
    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        int result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) {
            return;  // 종료됨
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 타임아웃 — SIGKILL
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
#endif
}

bool ChildProcessManager::can_restart_child(const ChildProcessInfo& info) const {
    if (info.config.max_restarts <= 0) return true;  // 무제한

    if (info.restart_count >= info.config.max_restarts) {
        // 윈도우 체크: restart_window 이내에 max_restarts를 초과했는지
        auto elapsed = std::chrono::steady_clock::now() - info.last_restart_time;
        auto window = std::chrono::seconds(info.config.restart_window_sec);
        if (elapsed > window) {
            // 윈도우가 지났으면 카운터 리셋 (const이므로 여기서는 true만 반환)
            return true;
        }
        return false;
    }

    return true;
}

}  // namespace arbitrage
