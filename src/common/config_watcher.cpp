/**
 * Config Watcher Implementation (TASK_15)
 */

#include "arbitrage/common/config_watcher.hpp"

#include <iostream>
#include <fstream>

namespace arbitrage {

// =============================================================================
// 글로벌 인스턴스
// =============================================================================
ConfigWatcher& config_watcher() {
    static ConfigWatcher instance("config/config.yaml");
    return instance;
}

// =============================================================================
// ConfigWatcher 생성자/소멸자
// =============================================================================
ConfigWatcher::ConfigWatcher(
    const std::string& config_path,
    std::chrono::milliseconds check_interval
)
    : config_path_(config_path)
    , check_interval_(check_interval)
{
    // 초기 파일 수정 시각 저장
    if (std::filesystem::exists(config_path_)) {
        last_modified_ = get_file_mtime();
    }
}

ConfigWatcher::~ConfigWatcher() {
    stop();
}

// =============================================================================
// 감시 제어
// =============================================================================
void ConfigWatcher::start() {
    if (running_.load()) return;

    running_ = true;
    watch_thread_ = std::thread(&ConfigWatcher::watch_loop, this);
}

void ConfigWatcher::stop() {
    running_ = false;

    if (watch_thread_.joinable()) {
        watch_thread_.join();
    }
}

bool ConfigWatcher::reload() {
    return do_reload();
}

// =============================================================================
// 콜백 설정
// =============================================================================
void ConfigWatcher::on_reload(ReloadCallback cb) {
    std::unique_lock lock(callback_mutex_);
    reload_callbacks_.push_back(std::move(cb));
}

void ConfigWatcher::on_error(ErrorCallback cb) {
    std::unique_lock lock(callback_mutex_);
    error_callbacks_.push_back(std::move(cb));
}

void ConfigWatcher::on_change(ChangeCallback cb) {
    std::unique_lock lock(callback_mutex_);
    change_callbacks_.push_back(std::move(cb));
}

// =============================================================================
// 설정 경로 변경
// =============================================================================
void ConfigWatcher::set_config_path(const std::string& path) {
    std::unique_lock lock(path_mutex_);
    config_path_ = path;

    // 새 파일의 수정 시각 저장
    if (std::filesystem::exists(path)) {
        last_modified_ = std::filesystem::last_write_time(path);
    }
}

// =============================================================================
// 감시 루프
// =============================================================================
void ConfigWatcher::watch_loop() {
    while (running_.load()) {
        ++stats_.check_count;

        if (check_modified()) {
            do_reload();
        }

        std::this_thread::sleep_for(check_interval_);
    }
}

// =============================================================================
// 파일 변경 확인
// =============================================================================
bool ConfigWatcher::check_modified() {
    try {
        std::shared_lock lock(path_mutex_);

        if (!std::filesystem::exists(config_path_)) {
            return false;
        }

        auto current_mtime = get_file_mtime();

        if (current_mtime != last_modified_) {
            last_modified_ = current_mtime;
            return true;
        }
    } catch (const std::exception& e) {
        ++stats_.error_count;

        // 오류 콜백 호출
        std::shared_lock lock(callback_mutex_);
        for (const auto& cb : error_callbacks_) {
            cb(std::string("Failed to check file modification: ") + e.what());
        }
    }

    return false;
}

// =============================================================================
// 설정 리로드 실행
// =============================================================================
bool ConfigWatcher::do_reload() {
    ConfigChangeEvent event;
    {
        std::shared_lock lock(path_mutex_);
        event.file_path = config_path_;
    }
    event.timestamp = std::chrono::system_clock::now();

    try {
        // 파일 존재 확인
        std::shared_lock path_lock(path_mutex_);
        if (!std::filesystem::exists(config_path_)) {
            throw std::runtime_error("Config file not found: " + config_path_);
        }
        path_lock.unlock();

        // 설정 리로드 성공으로 간주 (실제 Config 로드는 콜백에서 처리)
        last_reload_time_ = event.timestamp;
        ++stats_.reload_count;
        event.success = true;

        // 리로드 콜백 호출 - 실제 설정 리로드는 콜백에서 수행
        {
            std::shared_lock lock(callback_mutex_);
            for (const auto& cb : reload_callbacks_) {
                cb();
            }
        }

    } catch (const std::exception& e) {
        ++stats_.error_count;
        event.success = false;
        event.error_message = e.what();

        // 오류 콜백 호출
        std::shared_lock lock(callback_mutex_);
        for (const auto& cb : error_callbacks_) {
            cb(std::string("Failed to reload config: ") + e.what());
        }
    }

    // 변경 이벤트 콜백 호출
    {
        std::shared_lock lock(callback_mutex_);
        for (const auto& cb : change_callbacks_) {
            cb(event);
        }
    }

    return event.success;
}

// =============================================================================
// 파일 수정 시각 조회
// =============================================================================
std::filesystem::file_time_type ConfigWatcher::get_file_mtime() const {
    // 호출자가 path_mutex_를 이미 잠근 상태에서 호출되어야 함
    return std::filesystem::last_write_time(config_path_);
}

// =============================================================================
// MultiConfigWatcher 구현
// =============================================================================
MultiConfigWatcher::MultiConfigWatcher(std::chrono::milliseconds check_interval)
    : check_interval_(check_interval)
{
}

MultiConfigWatcher::~MultiConfigWatcher() {
    stop();
}

void MultiConfigWatcher::add_file(const std::string& path, std::function<void()> callback) {
    std::unique_lock lock(files_mutex_);

    // 중복 확인
    for (const auto& wf : watched_files_) {
        if (wf.path == path) {
            return;  // 이미 감시 중
        }
    }

    WatchedFile wf;
    wf.path = path;
    wf.callback = std::move(callback);

    if (std::filesystem::exists(path)) {
        wf.last_modified = std::filesystem::last_write_time(path);
    }

    watched_files_.push_back(std::move(wf));
}

void MultiConfigWatcher::remove_file(const std::string& path) {
    std::unique_lock lock(files_mutex_);

    watched_files_.erase(
        std::remove_if(
            watched_files_.begin(),
            watched_files_.end(),
            [&path](const WatchedFile& wf) { return wf.path == path; }
        ),
        watched_files_.end()
    );
}

std::vector<std::string> MultiConfigWatcher::watched_files() const {
    std::shared_lock lock(files_mutex_);
    std::vector<std::string> paths;
    paths.reserve(watched_files_.size());
    for (const auto& wf : watched_files_) {
        paths.push_back(wf.path);
    }
    return paths;
}

void MultiConfigWatcher::start() {
    if (running_.load()) return;

    running_ = true;
    watch_thread_ = std::thread(&MultiConfigWatcher::watch_loop, this);
}

void MultiConfigWatcher::stop() {
    running_ = false;

    if (watch_thread_.joinable()) {
        watch_thread_.join();
    }
}

void MultiConfigWatcher::watch_loop() {
    while (running_.load()) {
        {
            std::shared_lock lock(files_mutex_);

            for (auto& wf : watched_files_) {
                try {
                    if (!std::filesystem::exists(wf.path)) {
                        continue;
                    }

                    auto current_mtime = std::filesystem::last_write_time(wf.path);

                    if (current_mtime != wf.last_modified) {
                        wf.last_modified = current_mtime;

                        // 콜백 호출
                        if (wf.callback) {
                            wf.callback();
                        }
                    }
                } catch (const std::exception& e) {
                    // 오류 무시 (파일 접근 실패 등)
                }
            }
        }

        std::this_thread::sleep_for(check_interval_);
    }
}

}  // namespace arbitrage
