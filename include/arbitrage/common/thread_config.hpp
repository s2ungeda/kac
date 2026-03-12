#pragma once

/**
 * Thread Configuration Types (TASK_20)
 *
 * 스레드 어피니티, 우선순위 설정을 위한 타입 정의
 */

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>
#include <thread>

namespace arbitrage {

// =============================================================================
// 스레드 우선순위
// =============================================================================
enum class ThreadPriority : uint8_t {
    Idle,       // 최저 (백그라운드 작업)
    Low,        // 낮음 (로깅, 메트릭)
    Normal,     // 보통
    High,       // 높음 (시세 수신)
    RealTime    // 최고 (주문 실행) - 주의: root/CAP_SYS_NICE 필요
};

/**
 * 우선순위 문자열 변환
 */
inline const char* to_string(ThreadPriority priority) {
    switch (priority) {
        case ThreadPriority::Idle:     return "idle";
        case ThreadPriority::Low:      return "low";
        case ThreadPriority::Normal:   return "normal";
        case ThreadPriority::High:     return "high";
        case ThreadPriority::RealTime: return "realtime";
        default:                       return "unknown";
    }
}

/**
 * 문자열에서 우선순위 파싱
 */
inline ThreadPriority parse_priority(const std::string& str) {
    if (str == "idle")     return ThreadPriority::Idle;
    if (str == "low")      return ThreadPriority::Low;
    if (str == "normal")   return ThreadPriority::Normal;
    if (str == "high")     return ThreadPriority::High;
    if (str == "realtime") return ThreadPriority::RealTime;
    return ThreadPriority::Normal;
}

// =============================================================================
// 개별 스레드 설정
// =============================================================================
struct ThreadConfig {
    std::string name;                           // 스레드 이름 (디버깅용)
    std::optional<int> core_id;                 // 코어 ID (nullopt = 자동)
    std::optional<std::vector<int>> core_set;   // 복수 코어 허용 시
    ThreadPriority priority = ThreadPriority::Normal;
    size_t stack_size = 0;                      // 0 = 기본값
    std::optional<int> numa_node;               // NUMA 노드 (nullopt = 자동)

    ThreadConfig() = default;

    ThreadConfig(const std::string& n, ThreadPriority p = ThreadPriority::Normal)
        : name(n), priority(p) {}

    ThreadConfig(const std::string& n, int core, ThreadPriority p = ThreadPriority::Normal)
        : name(n), core_id(core), priority(p) {}
};

// =============================================================================
// 전체 스레드 매니저 설정
// =============================================================================
struct ThreadManagerConfig {
    bool affinity_enabled = true;       // 코어 어피니티 활성화
    bool priority_enabled = true;       // 우선순위 설정 활성화
    bool numa_enabled = false;          // NUMA 인식 활성화

    // 스레드별 설정 (이름 -> 설정)
    std::map<std::string, ThreadConfig> threads;

    // 기본 설정 (이름이 없는 스레드용)
    ThreadConfig default_config;

    /**
     * 스레드 설정 조회 (없으면 기본값 반환)
     */
    const ThreadConfig& get_config(const std::string& name) const {
        auto it = threads.find(name);
        if (it != threads.end()) {
            return it->second;
        }
        return default_config;
    }

    /**
     * 스레드 설정 추가/수정
     */
    void set_config(const std::string& name, const ThreadConfig& config) {
        threads[name] = config;
    }
};

// =============================================================================
// 스레드 통계
// =============================================================================
struct ThreadStats {
    std::string name;
    std::thread::id thread_id;
    int current_core = -1;              // 현재 실행 중인 코어 (-1 = 미확인)
    ThreadPriority priority = ThreadPriority::Normal;
    uint64_t context_switches = 0;      // 컨텍스트 스위치 횟수
    uint64_t cpu_time_us = 0;           // CPU 사용 시간 (마이크로초)
    double cpu_usage_pct = 0.0;         // CPU 사용률 (%)
    bool is_alive = false;              // 스레드 활성 여부
};

// =============================================================================
// 시스템 정보
// =============================================================================
struct SystemTopology {
    int logical_cores = 0;              // 논리 코어 수 (HT 포함)
    int physical_cores = 0;             // 물리 코어 수
    int numa_nodes = 1;                 // NUMA 노드 수
    bool hyperthreading = false;        // HT 활성화 여부
    std::vector<int> physical_core_ids; // 물리 코어 ID 목록

    // NUMA 노드별 코어 목록
    std::map<int, std::vector<int>> numa_node_cores;
};

}  // namespace arbitrage
