#pragma once

/**
 * Config Watcher (TASK_15)
 *
 * 런타임 설정 파일 변경 감지 및 핫 리로드
 * - 파일 변경 감지 (mtime 기반)
 * - 자동 리로드
 * - 콜백 알림
 * - 검증 후 적용
 */

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <shared_mutex>
#include <filesystem>

namespace arbitrage {

// =============================================================================
// 설정 변경 이벤트
// =============================================================================
struct ConfigChangeEvent {
    std::string file_path;
    std::chrono::system_clock::time_point timestamp;
    bool success{false};
    std::string error_message;
};

// =============================================================================
// 설정 감시자
// =============================================================================
class ConfigWatcher {
public:
    // 콜백 타입
    using ReloadCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;
    using ChangeCallback = std::function<void(const ConfigChangeEvent&)>;

    /**
     * 생성자
     * @param config_path 감시할 설정 파일 경로
     * @param check_interval 파일 변경 확인 주기
     */
    explicit ConfigWatcher(
        const std::string& config_path,
        std::chrono::milliseconds check_interval = std::chrono::milliseconds(1000)
    );

    ~ConfigWatcher();

    // 복사/이동 금지
    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    // =========================================================================
    // 감시 제어
    // =========================================================================

    /**
     * 감시 시작
     */
    void start();

    /**
     * 감시 중지
     */
    void stop();

    /**
     * 감시 중 여부
     */
    bool is_running() const { return running_.load(); }

    /**
     * 수동 리로드 트리거
     * @return 성공 여부
     */
    bool reload();

    // =========================================================================
    // 콜백 설정
    // =========================================================================

    /**
     * 리로드 성공 콜백 설정
     */
    void on_reload(ReloadCallback cb);

    /**
     * 오류 콜백 설정
     */
    void on_error(ErrorCallback cb);

    /**
     * 변경 이벤트 콜백 설정
     */
    void on_change(ChangeCallback cb);

    // =========================================================================
    // 설정 접근
    // =========================================================================

    /**
     * 설정 경로 조회
     */
    const std::string& config_path() const { return config_path_; }

    /**
     * 마지막 리로드 시각
     */
    std::chrono::system_clock::time_point last_reload_time() const {
        return last_reload_time_;
    }

    /**
     * 설정 경로 변경
     */
    void set_config_path(const std::string& path);

    /**
     * 확인 주기 변경
     */
    void set_check_interval(std::chrono::milliseconds interval) {
        check_interval_ = interval;
    }

    // =========================================================================
    // 통계
    // =========================================================================

    struct Stats {
        std::atomic<uint64_t> check_count{0};
        std::atomic<uint64_t> reload_count{0};
        std::atomic<uint64_t> error_count{0};

        void reset() {
            check_count = 0;
            reload_count = 0;
            error_count = 0;
        }
    };

    const Stats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }

private:
    // 감시 루프
    void watch_loop();

    // 파일 변경 확인
    bool check_modified();

    // 설정 리로드 실행
    bool do_reload();

    // 파일 수정 시각 조회
    std::filesystem::file_time_type get_file_mtime() const;

    // 멤버 변수
    mutable std::shared_mutex path_mutex_;
    std::string config_path_;
    std::chrono::milliseconds check_interval_;

    // 파일 상태
    std::filesystem::file_time_type last_modified_;
    std::chrono::system_clock::time_point last_reload_time_;

    // 감시 스레드
    std::atomic<bool> running_{false};
    std::thread watch_thread_;

    // 콜백
    mutable std::shared_mutex callback_mutex_;
    std::vector<ReloadCallback> reload_callbacks_;
    std::vector<ErrorCallback> error_callbacks_;
    std::vector<ChangeCallback> change_callbacks_;

    // 통계
    mutable Stats stats_;
};

// =============================================================================
// 다중 파일 감시자
// =============================================================================
class MultiConfigWatcher {
public:
    MultiConfigWatcher(std::chrono::milliseconds check_interval = std::chrono::milliseconds(1000));
    ~MultiConfigWatcher();

    // 복사/이동 금지
    MultiConfigWatcher(const MultiConfigWatcher&) = delete;
    MultiConfigWatcher& operator=(const MultiConfigWatcher&) = delete;

    // =========================================================================
    // 파일 관리
    // =========================================================================

    /**
     * 감시할 파일 추가
     * @param path 파일 경로
     * @param callback 변경 시 콜백
     */
    void add_file(const std::string& path, std::function<void()> callback);

    /**
     * 감시 파일 제거
     */
    void remove_file(const std::string& path);

    /**
     * 감시 파일 목록 조회
     */
    std::vector<std::string> watched_files() const;

    // =========================================================================
    // 감시 제어
    // =========================================================================

    void start();
    void stop();
    bool is_running() const { return running_.load(); }

private:
    struct WatchedFile {
        std::string path;
        std::filesystem::file_time_type last_modified;
        std::function<void()> callback;
    };

    void watch_loop();

    std::chrono::milliseconds check_interval_;
    std::atomic<bool> running_{false};
    std::thread watch_thread_;

    mutable std::shared_mutex files_mutex_;
    std::vector<WatchedFile> watched_files_;
};

// =============================================================================
// 글로벌 인스턴스 접근자
// =============================================================================
ConfigWatcher& config_watcher();

}  // namespace arbitrage
