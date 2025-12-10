# TASK 19: 설정 Hot-reload (C++)

## 🎯 목표
런타임 설정 파일 변경 감지 및 적용

---

## 📁 생성할 파일

```
include/arbitrage/common/
└── config_watcher.hpp
src/common/
└── config_watcher.cpp
```

---

## 📝 핵심 구현

```cpp
class ConfigWatcher {
public:
    ConfigWatcher(const std::string& config_path);
    
    // 감시 시작/중지
    void start();
    void stop();
    
    // 변경 콜백
    using ReloadCallback = std::function<void(const Config&)>;
    void on_reload(ReloadCallback cb);
    
private:
    void watch_loop();
    bool check_modified();
    
    std::string config_path_;
    std::filesystem::file_time_type last_modified_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> watch_thread_;
};
```

---

## ✅ 완료 조건

```
□ 파일 변경 감지
□ 자동 리로드
□ 콜백 알림
□ 검증 후 적용
```

---

## 📎 다음 태스크

완료 후: TASK_20_secrets_manager.md
