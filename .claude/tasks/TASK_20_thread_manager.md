# TASK 37: 스레드 매니저 (C++)

## 🎯 목표
스레드 어피니티, 우선순위, NUMA 인식 메모리 할당을 통한 저지연 최적화

---

## ⚠️ 핵심 설계 원칙

### 스레드 격리 (Thread Isolation)

```
┌─────────────────────────────────────────────────────────────────┐
│  물리 코어 격리 전략                                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  [네트워크 I/O 그룹]          [연산 그룹]                       │
│  ┌─────────────────┐          ┌─────────────────┐              │
│  │ Core 0: Upbit   │          │ Core 4: Strategy│              │
│  │ Core 1: Binance │   ───→   │ Core 5: Executor│              │
│  │ Core 2: Bithumb │ Lock-Free│                 │              │
│  │ Core 3: MEXC    │  Queue   │                 │              │
│  └─────────────────┘          └─────────────────┘              │
│                                                                 │
│  • I/O 스레드와 연산 스레드를 물리적으로 분리                    │
│  • 서로 다른 L2 캐시 사용 → 캐시 경합 없음                      │
│  • Lock-Free Queue로 데이터 전달 (TASK_39)                     │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 왜 물리 코어 분리인가?

```
하이퍼스레딩 주의:
- Core 0 (Thread 0, Thread 8) 은 L1/L2 캐시 공유
- I/O 스레드와 Strategy 스레드가 같은 물리 코어에 있으면:
  → 캐시 경합 → False Sharing → 성능 저하

권장 배치 (8코어 16스레드 기준):
- Thread 0~3: WebSocket I/O (물리 코어 0~3)
- Thread 4~5: Strategy/Executor (물리 코어 4~5)
- Thread 6~7: 유틸리티 (물리 코어 6~7)
- Thread 8~15: 사용하지 않거나 낮은 우선순위 작업
```

---

## ⚠️ 왜 필요한가?

### 스레드 어피니티
```
문제: OS가 스레드를 임의 코어로 이동
→ L1/L2 캐시 무효화 (Cache Miss)
→ 100배 느린 메모리 접근 발생

해결: 핵심 스레드를 특정 코어에 "고정"
→ 캐시 히트율 극대화, 지연 안정화
```

### 스레드 우선순위
```
문제: 로깅/메트릭 스레드가 CPU 점유
→ 주문 실행 스레드 지연

해결: 주문/전략 스레드에 높은 우선순위
→ OS 스케줄러가 우선 실행
```

### NUMA 인식
```
다중 CPU 서버에서:
- CPU 0 ↔ 메모리 0: 로컬 (빠름)
- CPU 0 ↔ 메모리 1: 리모트 (느림)

스레드와 데이터를 같은 NUMA 노드에 배치 필요
```

---

## 🔗 Lock-Free Queue 연계

```cpp
// WebSocket 스레드 (Core 0) → 전략 스레드 (Core 4)
// std::mutex 사용 금지! Lock-Free Queue 필수

#include "arbitrage/common/lockfree_queue.hpp"  // TASK_39

// 각 거래소별 SPSC Queue
SPSCQueue<TickerEvent> upbit_to_strategy{4096};    // Core 0 → Core 4
SPSCQueue<TickerEvent> binance_to_strategy{4096};  // Core 1 → Core 4

// 전략 → 주문 실행기
SPSCQueue<OrderCommand> strategy_to_executor{256}; // Core 4 → Core 5
```

---

## 📁 생성할 파일

```
include/arbitrage/common/
├── thread_manager.hpp
├── thread_config.hpp
└── numa_allocator.hpp
src/common/
├── thread_manager.cpp
├── thread_manager_linux.cpp
├── thread_manager_windows.cpp
└── numa_allocator.cpp
config/
└── threads.yaml
tests/unit/common/
└── thread_manager_test.cpp
```

---

## 📝 핵심 구현

### 1. 스레드 설정 타입 (thread_config.hpp)

```cpp
#pragma once

#include <string>
#include <vector>
#include <optional>

namespace arbitrage {

// 스레드 우선순위
enum class ThreadPriority {
    Idle,           // 최저 (백그라운드 작업)
    Low,            // 낮음 (로깅, 메트릭)
    Normal,         // 보통
    High,           // 높음 (시세 수신)
    RealTime        // 최고 (주문 실행) - 주의: root/admin 필요
};

// 개별 스레드 설정
struct ThreadConfig {
    std::string name;                       // 스레드 이름 (디버깅용)
    std::optional<int> core_id;             // 코어 ID (-1 or nullopt = 자동)
    std::optional<std::vector<int>> core_set; // 복수 코어 허용 시
    ThreadPriority priority = ThreadPriority::Normal;
    size_t stack_size = 0;                  // 0 = 기본값
    std::optional<int> numa_node;           // NUMA 노드 (-1 = 자동)
};

// 전체 스레드 설정
struct ThreadManagerConfig {
    bool affinity_enabled = true;
    bool priority_enabled = true;
    bool numa_enabled = false;
    
    // 스레드별 설정
    std::map<std::string, ThreadConfig> threads;
    
    // 기본값
    ThreadConfig default_config;
};

// 스레드 통계
struct ThreadStats {
    std::string name;
    int current_core;
    ThreadPriority priority;
    uint64_t context_switches;
    uint64_t cpu_time_us;
    double cpu_usage_pct;
};

}  // namespace arbitrage
```

### 2. 스레드 매니저 (thread_manager.hpp)

```cpp
#pragma once

#include "arbitrage/common/thread_config.hpp"
#include <thread>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>

namespace arbitrage {

class ThreadManager {
public:
    static ThreadManager& instance();
    
    // 초기화
    void initialize(const ThreadManagerConfig& config);
    void load_config(const std::string& config_path);
    
    // ★ 관리되는 스레드 생성
    template<typename Func, typename... Args>
    std::jthread create_thread(
        const std::string& thread_name,
        Func&& func,
        Args&&... args
    );
    
    // 기존 스레드에 설정 적용
    void apply_config(std::thread& t, const std::string& thread_name);
    void apply_config(std::jthread& t, const std::string& thread_name);
    
    // 현재 스레드에 설정 적용 (스레드 내부에서 호출)
    static void apply_to_current(const ThreadConfig& config);
    static void set_current_name(const std::string& name);
    static void set_current_affinity(int core_id);
    static void set_current_affinity(const std::vector<int>& core_set);
    static void set_current_priority(ThreadPriority priority);
    
    // 시스템 정보
    static int get_num_logical_cores();
    static int get_num_physical_cores();
    static int get_num_numa_nodes();
    static std::vector<int> get_cores_for_numa_node(int node);
    static bool is_hyperthreading_enabled();
    
    // 물리 코어만 반환 (HT 제외)
    static std::vector<int> get_physical_core_ids();
    
    // 모니터링
    std::vector<ThreadStats> get_all_stats() const;
    ThreadStats get_thread_stats(const std::string& name) const;
    
    // 런타임 조정
    void update_affinity(const std::string& thread_name, int core_id);
    void update_priority(const std::string& thread_name, ThreadPriority priority);
    
private:
    ThreadManager() = default;
    
    // 플랫폼별 구현 (pimpl)
    class Impl;
    std::unique_ptr<Impl> impl_;
    
    ThreadManagerConfig config_;
    std::map<std::string, std::thread::id> managed_threads_;
    mutable std::shared_mutex mutex_;
};

// 템플릿 구현
template<typename Func, typename... Args>
std::jthread ThreadManager::create_thread(
    const std::string& thread_name,
    Func&& func,
    Args&&... args
) {
    auto& cfg = config_.threads.count(thread_name) 
        ? config_.threads[thread_name] 
        : config_.default_config;
    
    return std::jthread([=, func = std::forward<Func>(func)]() mutable {
        // 스레드 시작 시 설정 적용
        apply_to_current(cfg);
        set_current_name(thread_name);
        
        // 실제 작업 실행
        func(std::forward<Args>(args)...);
    });
}

}  // namespace arbitrage
```

### 3. NUMA 인식 할당자 (numa_allocator.hpp)

```cpp
#pragma once

#include <memory>
#include <cstddef>

namespace arbitrage {

// NUMA 노드 지정 할당자
template<typename T>
class NumaAllocator {
public:
    using value_type = T;
    
    explicit NumaAllocator(int numa_node = -1) 
        : numa_node_(numa_node) {}
    
    template<typename U>
    NumaAllocator(const NumaAllocator<U>& other) 
        : numa_node_(other.numa_node()) {}
    
    T* allocate(std::size_t n);
    void deallocate(T* p, std::size_t n);
    
    int numa_node() const { return numa_node_; }
    
private:
    int numa_node_;
};

// NUMA 인식 버퍼 (오더북, 시세 데이터용)
class NumaBuffer {
public:
    NumaBuffer(size_t size, int numa_node = -1);
    ~NumaBuffer();
    
    void* data() { return data_; }
    const void* data() const { return data_; }
    size_t size() const { return size_; }
    int numa_node() const { return numa_node_; }
    
private:
    void* data_;
    size_t size_;
    int numa_node_;
};

// 헬퍼: 현재 스레드의 NUMA 노드
int get_current_numa_node();

// 헬퍼: 메모리 주소가 속한 NUMA 노드
int get_memory_numa_node(void* ptr);

}  // namespace arbitrage
```

### 4. 플랫폼별 구현 예시 (Linux)

```cpp
// src/common/thread_manager_linux.cpp

#include "arbitrage/common/thread_manager.hpp"
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <numa.h>  // libnuma

namespace arbitrage {

void ThreadManager::set_current_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    int rc = pthread_setaffinity_np(
        pthread_self(),
        sizeof(cpu_set_t),
        &cpuset
    );
    
    if (rc != 0) {
        throw std::runtime_error("Failed to set affinity");
    }
}

void ThreadManager::set_current_priority(ThreadPriority priority) {
    int policy;
    struct sched_param param;
    
    switch (priority) {
        case ThreadPriority::Idle:
            policy = SCHED_IDLE;
            param.sched_priority = 0;
            break;
        case ThreadPriority::Low:
            policy = SCHED_OTHER;
            param.sched_priority = 0;
            setpriority(PRIO_PROCESS, 0, 10);  // nice +10
            return;
        case ThreadPriority::Normal:
            policy = SCHED_OTHER;
            param.sched_priority = 0;
            break;
        case ThreadPriority::High:
            policy = SCHED_OTHER;
            param.sched_priority = 0;
            setpriority(PRIO_PROCESS, 0, -10);  // nice -10, needs CAP_SYS_NICE
            return;
        case ThreadPriority::RealTime:
            policy = SCHED_FIFO;
            param.sched_priority = 50;  // needs root or CAP_SYS_NICE
            break;
    }
    
    int rc = pthread_setschedparam(pthread_self(), policy, &param);
    if (rc != 0 && priority == ThreadPriority::RealTime) {
        // RealTime 실패 시 High로 폴백
        spdlog::warn("RealTime priority failed, falling back to High");
        set_current_priority(ThreadPriority::High);
    }
}

int ThreadManager::get_num_numa_nodes() {
    if (numa_available() < 0) {
        return 1;  // NUMA 미지원
    }
    return numa_max_node() + 1;
}

std::vector<int> ThreadManager::get_cores_for_numa_node(int node) {
    std::vector<int> cores;
    
    if (numa_available() < 0) {
        // NUMA 미지원 - 모든 코어 반환
        int num_cores = get_num_logical_cores();
        for (int i = 0; i < num_cores; ++i) {
            cores.push_back(i);
        }
        return cores;
    }
    
    struct bitmask* cpus = numa_allocate_cpumask();
    numa_node_to_cpus(node, cpus);
    
    int num_cores = get_num_logical_cores();
    for (int i = 0; i < num_cores; ++i) {
        if (numa_bitmask_isbitset(cpus, i)) {
            cores.push_back(i);
        }
    }
    
    numa_free_cpumask(cpus);
    return cores;
}

}  // namespace arbitrage
```

---

## 📊 권장 스레드 배치

```
┌─────────────────────────────────────────────────────────────┐
│  [NUMA Node 0 / 물리코어 0-3]                               │
│                                                             │
│  Core 0: upbit_websocket     (High)     ← 캐시 격리        │
│  Core 1: binance_websocket   (High)                        │
│  Core 2: strategy_engine     (RealTime) ← 핵심!            │
│  Core 3: order_executor      (RealTime) ← 핵심!            │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│  [NUMA Node 0 / 물리코어 4-7 또는 HT 코어]                   │
│                                                             │
│  Core 4: bithumb_websocket   (High)                        │
│  Core 5: mexc_websocket      (High)                        │
│  Core 6: tcp_server          (Normal)                      │
│  Core 7: logging, metrics    (Low)      ← 공유 가능        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 📝 설정 파일 (threads.yaml)

```yaml
thread_manager:
  affinity_enabled: true
  priority_enabled: true
  numa_enabled: false           # 단일 CPU면 false
  
  # 기본 설정
  default:
    priority: normal
    core_id: auto
    
  # 스레드별 설정
  threads:
    # ===== 시세 수신 (High Priority) =====
    upbit_websocket:
      core_id: 0
      priority: high
      
    binance_websocket:
      core_id: 1
      priority: high
      
    bithumb_websocket:
      core_id: 4
      priority: high
      
    mexc_websocket:
      core_id: 5
      priority: high
    
    # ===== 핵심 로직 (RealTime) =====
    strategy_engine:
      core_id: 2
      priority: realtime
      numa_node: 0              # 시세 데이터와 같은 노드
      
    order_executor:
      core_id: 3
      priority: realtime
      numa_node: 0
      
    # ===== 부가 기능 (Normal/Low) =====
    tcp_server:
      core_id: 6
      priority: normal
      
    event_bus:
      core_id: 6                # tcp_server와 공유 가능
      priority: normal
      
    logging:
      core_id: 7
      priority: low
      
    metrics:
      core_id: 7                # logging과 공유
      priority: low
      
    health_check:
      core_id: 7
      priority: low

  # NUMA 설정 (선택적)
  numa:
    enabled: false
    # 활성화 시:
    # node_0_threads: [upbit_ws, binance_ws, strategy, executor]
    # node_1_threads: [bithumb_ws, mexc_ws, tcp_server, logging]
```

---

## 🔗 의존성

```
TASK_01: Project Setup (CMake에 pthread, libnuma 추가)
TASK_19: Config Hot-reload (설정 파일 로드)
```

---

## ⚠️ 주의사항

```
1. RealTime 우선순위
   - Linux: CAP_SYS_NICE 또는 root 권한 필요
   - 설정 실패 시 High로 자동 폴백

2. 하이퍼스레딩
   - 핵심 스레드는 물리 코어에 배치 권장
   - Core 0,2,4,6 = 물리 / Core 1,3,5,7 = HT (예시)

3. 코어 수 확인
   - 설정된 core_id > 실제 코어 수 → 자동 할당 폴백
   
4. 클라우드/VM
   - vCPU 어피니티는 물리 코어 보장 안됨
   - 성능 민감 시 Dedicated Host 고려

5. 과도한 고정 금지
   - 스레드 > 코어 시 핵심만 고정
   - 나머지는 OS 스케줄러에 위임
```

---

## ✅ 완료 조건

```
□ 스레드 어피니티
  □ Linux (pthread_setaffinity_np)
  □ Windows (SetThreadAffinityMask)
  □ 단일/복수 코어 지정
  
□ 스레드 우선순위
  □ 5단계 (Idle ~ RealTime)
  □ 플랫폼별 매핑
  □ 권한 부족 시 폴백

□ NUMA 지원
  □ 노드 조회
  □ 노드별 코어 조회
  □ NumaAllocator (선택)

□ 설정 파일
  □ YAML 로드
  □ 런타임 갱신

□ 모니터링
  □ 스레드 통계 수집
  □ 현재 코어/우선순위 조회

□ 크로스 플랫폼
  □ Linux
  □ Windows

□ 단위 테스트
```

---

## 📎 다음 태스크

완료 후: TASK_35_fee_calculator.md (Phase 4 전략 시작)
