# TASK 25: Health Check (C++)

## 🎯 목표
시스템 상태 점검 및 모니터링

---

## 📁 생성할 파일

```
include/arbitrage/infra/
└── health_check.hpp
src/infra/
└── health_check.cpp
```

---

## 📝 핵심 구현

```cpp
enum class HealthStatus {
    Healthy,
    Degraded,
    Unhealthy
};

struct ComponentHealth {
    std::string name;
    HealthStatus status;
    std::string message;
    std::chrono::system_clock::time_point last_check;
};

struct SystemHealth {
    HealthStatus overall;
    std::vector<ComponentHealth> components;
    double cpu_usage;
    size_t memory_usage;
    std::chrono::system_clock::time_point timestamp;
};

class HealthChecker {
public:
    // 체크 함수 등록
    using CheckFunc = std::function<ComponentHealth()>;
    void register_check(const std::string& name, CheckFunc check);
    
    // 전체 상태 조회
    SystemHealth check_all();
    
    // 자동 체크 시작
    void start_periodic_check(std::chrono::seconds interval = std::chrono::seconds(30));
    void stop();
    
    // 콜백
    using AlertCallback = std::function<void(const ComponentHealth&)>;
    void on_unhealthy(AlertCallback cb);
};
```

---

## ✅ 완료 조건

```
□ 컴포넌트 상태 체크
□ 주기적 체크
□ 이상 시 알림
□ CPU/메모리 모니터링
```

---

## 📎 다음 태스크

완료 후: TASK_26_tcp_protocol.md
