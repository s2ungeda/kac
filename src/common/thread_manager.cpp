/**
 * Thread Manager Implementation (TASK_20)
 *
 * Linux 플랫폼 구현
 * - pthread API 사용
 * - NUMA 지원 (libnuma, 선택적)
 */

#include "arbitrage/common/thread_manager.hpp"
#include "arbitrage/common/logger.hpp"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <set>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace arbitrage {

namespace {
    auto logger() {
        static auto log = Logger::get("ThreadManager");
        return log;
    }
}

// =============================================================================
// Static 멤버 초기화
// =============================================================================
SystemTopology ThreadManager::cached_topology_;
std::once_flag ThreadManager::topology_init_flag_;

// =============================================================================
// 싱글톤
// =============================================================================
ThreadManager& ThreadManager::instance() {
    static ThreadManager inst;
    return inst;
}

ThreadManager::ThreadManager() {
    // 시스템 토폴로지 초기화
    std::call_once(topology_init_flag_, &ThreadManager::init_topology);
}

ThreadManager::~ThreadManager() = default;

// =============================================================================
// 초기화
// =============================================================================
void ThreadManager::initialize(const ThreadManagerConfig& config) {
    std::unique_lock lock(mutex_);
    config_ = config;

    logger()->info("ThreadManager initialized: affinity={}, priority={}, numa={}",
                   config_.affinity_enabled, config_.priority_enabled, config_.numa_enabled);
}

Result<void> ThreadManager::load_config(const std::string& /*config_path*/) {
    // NOTE: YAML 미사용 — 코드 기반 설정
    // 현재는 코드에서 직접 설정
    return Ok();
}

// =============================================================================
// 스레드 설정 적용
// =============================================================================
Result<void> ThreadManager::apply_config(std::thread& t, const std::string& thread_name) {
    if (!t.joinable()) {
        return Err<void>(ErrorCode::InvalidParameter, "Thread is not joinable");
    }

    const auto& cfg = config_.get_config(thread_name);

    // 스레드 핸들 획득
#ifdef __linux__
    pthread_t native_handle = t.native_handle();

    // 어피니티 설정
    if (config_.affinity_enabled && cfg.core_id.has_value()) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cfg.core_id.value(), &cpuset);

        int rc = pthread_setaffinity_np(native_handle, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            logger()->warn("Failed to set affinity for thread '{}' to core {}",
                          thread_name, cfg.core_id.value());
        }
    }
#endif

    // 스레드 등록
    register_thread(thread_name, t.get_id());

    return Ok();
}

// =============================================================================
// 현재 스레드 설정
// =============================================================================
Result<void> ThreadManager::apply_to_current(const ThreadConfig& config) {
    if (!config.name.empty()) {
        set_current_name(config.name);
    }

    if (config.core_id.has_value()) {
        auto result = set_current_affinity(config.core_id.value());
        if (result.has_error()) {
            return result;
        }
    } else if (config.core_set.has_value()) {
        auto result = set_current_affinity(config.core_set.value());
        if (result.has_error()) {
            return result;
        }
    }

    return set_current_priority(config.priority);
}

Result<void> ThreadManager::set_current_name(const std::string& name) {
#ifdef __linux__
    // Linux: 최대 15자
    std::string truncated = name.substr(0, 15);
    int rc = pthread_setname_np(pthread_self(), truncated.c_str());
    if (rc != 0) {
        return Err<void>(ErrorCode::SystemError, "pthread_setname_np failed");
    }
#endif

#ifdef _WIN32
    // Windows: SetThreadDescription (Windows 10+)
    // 여기서는 생략
#endif

    return Ok();
}

Result<void> ThreadManager::set_current_affinity(int core_id) {
#ifdef __linux__
    int num_cores = get_num_logical_cores();
    if (core_id < 0 || core_id >= num_cores) {
        return Err<void>(ErrorCode::InvalidParameter,
            "Invalid core_id: " + std::to_string(core_id));
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        return Err<void>(ErrorCode::SystemError,
            "pthread_setaffinity_np failed: " + std::to_string(rc));
    }

    logger()->debug("Set thread affinity to core {}", core_id);
#else
    (void)core_id;
#endif

#ifdef _WIN32
    DWORD_PTR mask = 1ULL << core_id;
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
        return Err<void>(ErrorCode::SystemError, "SetThreadAffinityMask failed");
    }
#endif

    return Ok();
}

Result<void> ThreadManager::set_current_affinity(const std::vector<int>& core_set) {
    if (core_set.empty()) {
        return Err<void>(ErrorCode::InvalidParameter, "Empty core set");
    }

#ifdef __linux__
    int num_cores = get_num_logical_cores();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int core_id : core_set) {
        if (core_id >= 0 && core_id < num_cores) {
            CPU_SET(core_id, &cpuset);
        }
    }

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        return Err<void>(ErrorCode::SystemError,
            "pthread_setaffinity_np failed: " + std::to_string(rc));
    }
#endif

#ifdef _WIN32
    DWORD_PTR mask = 0;
    for (int core_id : core_set) {
        mask |= (1ULL << core_id);
    }
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
        return Err<void>(ErrorCode::SystemError, "SetThreadAffinityMask failed");
    }
#endif

    return Ok();
}

Result<void> ThreadManager::set_current_priority(ThreadPriority priority) {
#ifdef __linux__
    int policy;
    struct sched_param param;
    param.sched_priority = 0;

    switch (priority) {
        case ThreadPriority::Idle:
            // SCHED_IDLE은 일부 시스템에서 미지원
            policy = SCHED_OTHER;
            if (setpriority(PRIO_PROCESS, 0, 19) != 0) {
                logger()->warn("Failed to set idle priority");
            }
            break;

        case ThreadPriority::Low:
            policy = SCHED_OTHER;
            if (setpriority(PRIO_PROCESS, 0, 10) != 0) {
                logger()->warn("Failed to set low priority (nice +10)");
            }
            break;

        case ThreadPriority::Normal:
            policy = SCHED_OTHER;
            // 기본값 유지
            break;

        case ThreadPriority::High:
            policy = SCHED_OTHER;
            // nice -10은 CAP_SYS_NICE 필요
            if (setpriority(PRIO_PROCESS, 0, -10) != 0) {
                logger()->debug("High priority requires CAP_SYS_NICE, using normal");
            }
            break;

        case ThreadPriority::RealTime:
            // SCHED_FIFO는 root 또는 CAP_SYS_NICE 필요
            policy = SCHED_FIFO;
            param.sched_priority = 50;

            if (pthread_setschedparam(pthread_self(), policy, &param) != 0) {
                // 실패 시 High로 폴백
                logger()->warn("RealTime priority failed, falling back to High");
                return set_current_priority(ThreadPriority::High);
            }
            return Ok();
    }

    // SCHED_OTHER는 priority 0
    if (policy == SCHED_OTHER) {
        pthread_setschedparam(pthread_self(), policy, &param);
    }
#endif

#ifdef _WIN32
    int win_priority;
    switch (priority) {
        case ThreadPriority::Idle:
            win_priority = THREAD_PRIORITY_IDLE;
            break;
        case ThreadPriority::Low:
            win_priority = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case ThreadPriority::Normal:
            win_priority = THREAD_PRIORITY_NORMAL;
            break;
        case ThreadPriority::High:
            win_priority = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case ThreadPriority::RealTime:
            win_priority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
    }

    if (!SetThreadPriority(GetCurrentThread(), win_priority)) {
        return Err<void>(ErrorCode::SystemError, "SetThreadPriority failed");
    }
#endif

    logger()->debug("Set thread priority to {}", to_string(priority));
    return Ok();
}

int ThreadManager::get_current_core() {
#ifdef __linux__
    return sched_getcpu();
#else
    return -1;
#endif
}

// =============================================================================
// 시스템 정보
// =============================================================================
void ThreadManager::init_topology() {
    cached_topology_.logical_cores = std::thread::hardware_concurrency();
    cached_topology_.physical_cores = cached_topology_.logical_cores;
    cached_topology_.numa_nodes = 1;
    cached_topology_.hyperthreading = false;

#ifdef __linux__
    // /proc/cpuinfo에서 물리 코어 수 파악
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::string line;
        std::set<std::string> physical_ids;
        std::set<std::string> core_ids;

        while (std::getline(cpuinfo, line)) {
            if (line.find("physical id") != std::string::npos) {
                physical_ids.insert(line);
            }
            if (line.find("core id") != std::string::npos) {
                core_ids.insert(line);
            }
        }

        if (!core_ids.empty()) {
            cached_topology_.physical_cores = core_ids.size();
            cached_topology_.hyperthreading =
                (cached_topology_.logical_cores > cached_topology_.physical_cores);
        }
    }

    // 물리 코어 ID 목록 (단순 추정: 짝수 코어)
    for (int i = 0; i < cached_topology_.logical_cores; ++i) {
        if (!cached_topology_.hyperthreading || (i < cached_topology_.physical_cores)) {
            cached_topology_.physical_core_ids.push_back(i);
        }
    }

    // NUMA 노드 수 확인
    std::ifstream numa_nodes("/sys/devices/system/node/online");
    if (numa_nodes.is_open()) {
        std::string line;
        std::getline(numa_nodes, line);
        // "0-1" 또는 "0" 형식 파싱
        size_t dash = line.find('-');
        if (dash != std::string::npos) {
            int last_node = std::stoi(line.substr(dash + 1));
            cached_topology_.numa_nodes = last_node + 1;
        } else if (!line.empty()) {
            cached_topology_.numa_nodes = 1;
        }
    }
#endif

    logger()->info("System topology: {} logical cores, {} physical cores, {} NUMA nodes, HT={}",
                   cached_topology_.logical_cores,
                   cached_topology_.physical_cores,
                   cached_topology_.numa_nodes,
                   cached_topology_.hyperthreading);
}

SystemTopology ThreadManager::get_system_topology() {
    std::call_once(topology_init_flag_, &ThreadManager::init_topology);
    return cached_topology_;
}

int ThreadManager::get_num_logical_cores() {
    std::call_once(topology_init_flag_, &ThreadManager::init_topology);
    return cached_topology_.logical_cores;
}

int ThreadManager::get_num_physical_cores() {
    std::call_once(topology_init_flag_, &ThreadManager::init_topology);
    return cached_topology_.physical_cores;
}

int ThreadManager::get_num_numa_nodes() {
    std::call_once(topology_init_flag_, &ThreadManager::init_topology);
    return cached_topology_.numa_nodes;
}

std::vector<int> ThreadManager::get_cores_for_numa_node(int node) {
    std::call_once(topology_init_flag_, &ThreadManager::init_topology);

    auto it = cached_topology_.numa_node_cores.find(node);
    if (it != cached_topology_.numa_node_cores.end()) {
        return it->second;
    }

    // NUMA 정보 없으면 모든 코어 반환
    std::vector<int> all_cores;
    for (int i = 0; i < cached_topology_.logical_cores; ++i) {
        all_cores.push_back(i);
    }
    return all_cores;
}

bool ThreadManager::is_hyperthreading_enabled() {
    std::call_once(topology_init_flag_, &ThreadManager::init_topology);
    return cached_topology_.hyperthreading;
}

std::vector<int> ThreadManager::get_physical_core_ids() {
    std::call_once(topology_init_flag_, &ThreadManager::init_topology);
    return cached_topology_.physical_core_ids;
}

// =============================================================================
// 모니터링
// =============================================================================
void ThreadManager::register_thread(const std::string& name, std::thread::id id) {
    std::unique_lock lock(mutex_);
    thread_ids_[name] = id;
    active_configs_[name] = config_.get_config(name);
}

void ThreadManager::unregister_thread(const std::string& name) {
    std::unique_lock lock(mutex_);
    thread_ids_.erase(name);
    active_configs_.erase(name);
}

std::vector<ThreadStats> ThreadManager::get_all_stats() const {
    std::shared_lock lock(mutex_);

    std::vector<ThreadStats> stats;
    stats.reserve(thread_ids_.size());

    for (const auto& [name, id] : thread_ids_) {
        ThreadStats s;
        s.name = name;
        s.thread_id = id;
        s.is_alive = true;

        auto cfg_it = active_configs_.find(name);
        if (cfg_it != active_configs_.end()) {
            s.priority = cfg_it->second.priority;
            if (cfg_it->second.core_id.has_value()) {
                s.current_core = cfg_it->second.core_id.value();
            }
        }

        stats.push_back(s);
    }

    return stats;
}

ThreadStats ThreadManager::get_thread_stats(const std::string& name) const {
    std::shared_lock lock(mutex_);

    ThreadStats s;
    s.name = name;
    s.is_alive = false;

    auto id_it = thread_ids_.find(name);
    if (id_it != thread_ids_.end()) {
        s.thread_id = id_it->second;
        s.is_alive = true;

        auto cfg_it = active_configs_.find(name);
        if (cfg_it != active_configs_.end()) {
            s.priority = cfg_it->second.priority;
            if (cfg_it->second.core_id.has_value()) {
                s.current_core = cfg_it->second.core_id.value();
            }
        }
    }

    return s;
}

// =============================================================================
// 런타임 조정
// =============================================================================
Result<void> ThreadManager::update_affinity(const std::string& thread_name, int core_id) {
    std::unique_lock lock(mutex_);

    auto it = thread_ids_.find(thread_name);
    if (it == thread_ids_.end()) {
        return Err<void>(ErrorCode::NotFound, "Thread not found: " + thread_name);
    }

    // 설정 업데이트
    active_configs_[thread_name].core_id = core_id;

    // NOTE: 실행 중 어피니티 변경은 ThreadManager::apply_to_current() 사용
    // 현재는 스레드가 직접 호출해야 함

    return Ok();
}

Result<void> ThreadManager::update_priority(const std::string& thread_name, ThreadPriority priority) {
    std::unique_lock lock(mutex_);

    auto it = thread_ids_.find(thread_name);
    if (it == thread_ids_.end()) {
        return Err<void>(ErrorCode::NotFound, "Thread not found: " + thread_name);
    }

    // 설정 업데이트
    active_configs_[thread_name].priority = priority;

    return Ok();
}

}  // namespace arbitrage
