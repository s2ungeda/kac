#pragma once

/**
 * Thread Manager (TASK_20)
 *
 * 스레드 어피니티, 우선순위, NUMA 인식 메모리 할당을 통한 저지연 최적화
 * - 스레드 생성 및 관리
 * - 코어 어피니티 설정
 * - 우선순위 설정
 * - 시스템 토폴로지 조회
 */

#include "arbitrage/common/thread_config.hpp"
#include "arbitrage/common/error.hpp"

#include <thread>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <atomic>
#include <mutex>

namespace arbitrage {

class ThreadManager {
public:
    /**
     * 싱글톤 인스턴스
     */
    static ThreadManager& instance();

    ThreadManager();
    ~ThreadManager();

    ThreadManager(const ThreadManager&) = delete;
    ThreadManager& operator=(const ThreadManager&) = delete;

    // =========================================================================
    // 초기화
    // =========================================================================

    /**
     * 설정으로 초기화
     */
    void initialize(const ThreadManagerConfig& config);

    /**
     * YAML 설정 파일 로드
     */
    Result<void> load_config(const std::string& config_path);

    /**
     * 현재 설정 반환
     */
    const ThreadManagerConfig& config() const { return config_; }

    // =========================================================================
    // 스레드 생성
    // =========================================================================

    /**
     * 관리되는 스레드 생성
     * 설정에 따라 어피니티, 우선순위 자동 적용
     */
    template<typename Func, typename... Args>
    std::thread create_thread(
        const std::string& thread_name,
        Func&& func,
        Args&&... args
    );

    /**
     * 기존 스레드에 설정 적용
     */
    Result<void> apply_config(std::thread& t, const std::string& thread_name);

    // =========================================================================
    // 현재 스레드 설정
    // =========================================================================

    /**
     * 현재 스레드에 설정 적용
     */
    static Result<void> apply_to_current(const ThreadConfig& config);

    /**
     * 현재 스레드 이름 설정
     */
    static Result<void> set_current_name(const std::string& name);

    /**
     * 현재 스레드 코어 어피니티 설정 (단일 코어)
     */
    static Result<void> set_current_affinity(int core_id);

    /**
     * 현재 스레드 코어 어피니티 설정 (복수 코어)
     */
    static Result<void> set_current_affinity(const std::vector<int>& core_set);

    /**
     * 현재 스레드 우선순위 설정
     */
    static Result<void> set_current_priority(ThreadPriority priority);

    /**
     * 현재 스레드 실행 중인 코어 ID 조회
     */
    static int get_current_core();

    // =========================================================================
    // 시스템 정보
    // =========================================================================

    /**
     * 시스템 토폴로지 조회
     */
    static SystemTopology get_system_topology();

    /**
     * 논리 코어 수 (HT 포함)
     */
    static int get_num_logical_cores();

    /**
     * 물리 코어 수
     */
    static int get_num_physical_cores();

    /**
     * NUMA 노드 수
     */
    static int get_num_numa_nodes();

    /**
     * NUMA 노드별 코어 목록
     */
    static std::vector<int> get_cores_for_numa_node(int node);

    /**
     * 하이퍼스레딩 활성화 여부
     */
    static bool is_hyperthreading_enabled();

    /**
     * 물리 코어 ID 목록 (HT 제외)
     */
    static std::vector<int> get_physical_core_ids();

    // =========================================================================
    // 모니터링
    // =========================================================================

    /**
     * 모든 관리 스레드 통계
     */
    std::vector<ThreadStats> get_all_stats() const;

    /**
     * 특정 스레드 통계
     */
    ThreadStats get_thread_stats(const std::string& name) const;

    /**
     * 스레드 등록 (이름 -> ID 매핑)
     */
    void register_thread(const std::string& name, std::thread::id id);

    /**
     * 스레드 등록 해제
     */
    void unregister_thread(const std::string& name);

    // =========================================================================
    // 런타임 조정
    // =========================================================================

    /**
     * 스레드 어피니티 변경
     */
    Result<void> update_affinity(const std::string& thread_name, int core_id);

    /**
     * 스레드 우선순위 변경
     */
    Result<void> update_priority(const std::string& thread_name, ThreadPriority priority);

private:
    ThreadManagerConfig config_;

    // 스레드 추적
    mutable std::shared_mutex mutex_;
    std::map<std::string, std::thread::id> thread_ids_;
    std::map<std::string, ThreadConfig> active_configs_;

    // 시스템 토폴로지 캐시
    static SystemTopology cached_topology_;
    static std::once_flag topology_init_flag_;
    static void init_topology();
};

// =============================================================================
// 템플릿 구현
// =============================================================================

template<typename Func, typename... Args>
std::thread ThreadManager::create_thread(
    const std::string& thread_name,
    Func&& func,
    Args&&... args
) {
    const auto& cfg = config_.get_config(thread_name);

    // 실제 함수와 인자를 캡처
    auto bound_func = std::bind(std::forward<Func>(func), std::forward<Args>(args)...);

    return std::thread([this, thread_name, cfg, bound_func = std::move(bound_func)]() mutable {
        // 스레드 시작 시 설정 적용
        ThreadConfig local_cfg = cfg;
        local_cfg.name = thread_name;

        // 이름 설정
        set_current_name(thread_name);

        // 어피니티 설정
        if (config_.affinity_enabled && local_cfg.core_id.has_value()) {
            set_current_affinity(local_cfg.core_id.value());
        } else if (config_.affinity_enabled && local_cfg.core_set.has_value()) {
            set_current_affinity(local_cfg.core_set.value());
        }

        // 우선순위 설정
        if (config_.priority_enabled) {
            set_current_priority(local_cfg.priority);
        }

        // 스레드 등록
        register_thread(thread_name, std::this_thread::get_id());

        // 실제 작업 실행
        bound_func();

        // 스레드 등록 해제
        unregister_thread(thread_name);
    });
}

// =============================================================================
// 글로벌 접근자
// =============================================================================

/**
 * ThreadManager 인스턴스 접근
 */
inline ThreadManager& thread_manager() {
    return ThreadManager::instance();
}

}  // namespace arbitrage
