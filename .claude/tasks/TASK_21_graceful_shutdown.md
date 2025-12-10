# TASK 24: Graceful Shutdown (C++)

## 🎯 목표
안전한 시스템 종료 처리

---

## 📁 생성할 파일

```
include/arbitrage/infra/
└── shutdown.hpp
src/infra/
└── shutdown.cpp
```

---

## 📝 핵심 구현

```cpp
class ShutdownManager {
public:
    static ShutdownManager& instance();
    
    // 시그널 핸들러 등록
    void install_signal_handlers();
    
    // 컴포넌트 등록 (종료 순서 역순)
    using ShutdownCallback = std::function<void()>;
    void register_component(const std::string& name, ShutdownCallback cb);
    
    // 종료 시작
    void initiate_shutdown();
    
    // 종료 대기
    bool wait_for_shutdown(Duration timeout = std::chrono::seconds(30));
    
    // 종료 상태
    bool is_shutting_down() const { return shutting_down_.load(); }
    
private:
    static void signal_handler(int signum);
    
    std::atomic<bool> shutting_down_{false};
    std::vector<std::pair<std::string, ShutdownCallback>> components_;
    std::mutex mutex_;
};
```

---

## ✅ 완료 조건

```
□ SIGINT/SIGTERM 처리
□ 컴포넌트 순차 종료
□ 타임아웃 처리
□ 열린 주문 처리
□ 로깅
```

---

## 📎 다음 태스크

완료 후: TASK_37_thread_manager.md
