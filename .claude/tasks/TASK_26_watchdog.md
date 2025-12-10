# TASK 38: 워치독 프로세스 (C++)

## 🎯 목표
메인 트레이딩 프로세스 감시 및 장애 복구 - 자동 재시작, 상태 영속화, 이상 탐지

---

## ⚠️ 왜 필요한가?

### 단일 프로세스의 위험
```
메인 프로세스 크래시 시나리오:
1. WebSocket 파싱 에러 → segfault
2. 메모리 릭 누적 → OOM Kill
3. 무한 루프 → CPU 100%, 응답 불가
4. 교착 상태 → 주문 실행 멈춤

결과: 포지션 보유 중 시스템 다운 → 큰 손실 가능
```

### 워치독의 역할
```
┌─────────────┐         ┌─────────────┐
│  Watchdog   │ ──────► │   Main      │
│  Process    │ 감시    │  Process    │
│  (경량)     │ ◄────── │ (트레이딩)  │
└─────────────┘  하트비트 └─────────────┘
       │
       ▼
  이상 감지 시:
  1. 프로세스 재시작
  2. 상태 복구
  3. 알림 발송
```

---

## 📁 생성할 파일

```
# 워치독 (별도 실행파일)
include/watchdog/
├── watchdog.hpp
├── process_monitor.hpp
├── state_persistence.hpp
└── health_protocol.hpp
src/watchdog/
├── main.cpp                    # 워치독 진입점
├── watchdog.cpp
├── process_monitor.cpp
├── process_monitor_linux.cpp
├── process_monitor_windows.cpp
└── state_persistence.cpp

# 메인 프로세스 측 (하트비트 전송)
include/arbitrage/infra/
└── watchdog_client.hpp
src/infra/
└── watchdog_client.cpp

config/
└── watchdog.yaml
```

---

## 📝 핵심 구현

### 1. 헬스 프로토콜 (health_protocol.hpp)

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <chrono>

namespace watchdog {

// 하트비트 메시지 (메인 → 워치독)
struct Heartbeat {
    uint64_t sequence;
    uint64_t timestamp_us;              // microseconds since epoch
    
    // 상태 정보
    uint32_t active_connections;        // WebSocket 연결 수
    uint32_t pending_orders;            // 대기 중인 주문
    uint64_t memory_usage_bytes;        // 메모리 사용량
    double cpu_usage_pct;               // CPU 사용률
    
    // 컴포넌트 상태
    uint8_t component_status;           // 비트 플래그
    // bit 0: WebSocket OK
    // bit 1: Strategy OK
    // bit 2: Executor OK
    // bit 3: TCP Server OK
    
    // 에러 카운트
    uint32_t error_count;
    uint32_t warning_count;
    
    bool is_healthy() const {
        return (component_status & 0x07) == 0x07;  // WS, Strategy, Executor OK
    }
};

// 명령 메시지 (워치독 → 메인)
enum class WatchdogCommand : uint8_t {
    None = 0,
    Shutdown = 1,           // 정상 종료 요청
    SaveState = 2,          // 상태 저장 요청
    ReloadConfig = 3,       // 설정 리로드
    KillSwitch = 4,         // 긴급 중단
    HealthCheck = 5         // 즉시 상태 보고 요청
};

struct CommandMessage {
    WatchdogCommand command;
    uint64_t timestamp_us;
    std::string payload;            // JSON 추가 데이터
};

// 통신 방식
// - Unix: Unix Domain Socket (/tmp/arbitrage_watchdog.sock)
// - Windows: Named Pipe (\\.\pipe\arbitrage_watchdog)
// - 대안: Shared Memory + Semaphore (더 빠름)

constexpr const char* SOCKET_PATH_LINUX = "/tmp/arbitrage_watchdog.sock";
constexpr const char* PIPE_NAME_WINDOWS = "\\\\.\\pipe\\arbitrage_watchdog";

}  // namespace watchdog
```

### 2. 프로세스 모니터 (process_monitor.hpp)

```cpp
#pragma once

#include <string>
#include <chrono>
#include <optional>
#include <functional>

namespace watchdog {

// 프로세스 상태
struct ProcessStatus {
    int pid;
    bool is_running;
    uint64_t memory_bytes;
    double cpu_percent;
    uint64_t uptime_sec;
    int exit_code;                      // 종료 시
    std::string exit_reason;            // 종료 사유
};

// 프로세스 모니터 (플랫폼별 구현)
class ProcessMonitor {
public:
    ProcessMonitor();
    ~ProcessMonitor();
    
    // 프로세스 시작
    struct LaunchConfig {
        std::string executable_path;
        std::vector<std::string> arguments;
        std::string working_directory;
        std::map<std::string, std::string> environment;
        bool redirect_output;           // stdout/stderr 캡처
        std::string log_file;           // 출력 로그 파일
    };
    
    int launch(const LaunchConfig& config);
    
    // 프로세스 제어
    bool terminate(int pid, int timeout_sec = 10);  // 정상 종료 시도
    bool kill(int pid);                             // 강제 종료
    bool is_running(int pid) const;
    
    // 상태 조회
    ProcessStatus get_status(int pid) const;
    
    // 리소스 모니터링
    uint64_t get_memory_usage(int pid) const;
    double get_cpu_usage(int pid) const;
    
    // 대기
    int wait_for_exit(int pid, int timeout_sec = -1);  // -1 = 무한 대기
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace watchdog
```

### 3. 상태 영속화 (state_persistence.hpp)

```cpp
#pragma once

#include <string>
#include <chrono>
#include <optional>

namespace watchdog {

// 저장할 상태 (크래시 복구용)
struct PersistedState {
    uint64_t version;
    std::chrono::system_clock::time_point saved_at;
    
    // 포지션 정보
    struct Position {
        std::string exchange;
        std::string symbol;
        double quantity;
        double avg_price;
        std::string side;               // "long" / "short"
    };
    std::vector<Position> open_positions;
    
    // 대기 중인 주문
    struct PendingOrder {
        std::string order_id;
        std::string exchange;
        std::string symbol;
        double quantity;
        double price;
        std::string side;
        std::string type;               // "limit" / "market"
        std::chrono::system_clock::time_point created_at;
    };
    std::vector<PendingOrder> pending_orders;
    
    // 전략 상태
    struct StrategyState {
        std::string strategy_id;
        std::string state_json;         // 전략별 커스텀 상태
        double pnl_today;
        int trades_today;
    };
    std::vector<StrategyState> strategies;
    
    // 통계
    double total_pnl_today;
    int total_trades_today;
    double daily_loss_used;             // 일일 손실 한도 사용량
    
    // 시스템 상태
    bool kill_switch_active;
    std::string last_error;
};

// 상태 저장/복구
class StatePersistence {
public:
    StatePersistence(const std::string& state_dir);
    
    // 저장 (주기적 호출)
    void save(const PersistedState& state);
    
    // 복구 (시작 시 호출)
    std::optional<PersistedState> load_latest();
    
    // 히스토리
    std::vector<std::string> list_snapshots(int max_count = 10);
    std::optional<PersistedState> load_snapshot(const std::string& snapshot_id);
    
    // 정리
    void cleanup_old_snapshots(int keep_count = 100);
    
private:
    std::string state_dir_;
    
    // 파일 형식: state_20241127_143022_001.dat
    std::string generate_filename() const;
    
    // 직렬화 (MessagePack 또는 Protobuf)
    std::vector<uint8_t> serialize(const PersistedState& state);
    PersistedState deserialize(const std::vector<uint8_t>& data);
};

}  // namespace watchdog
```

### 4. 워치독 메인 클래스 (watchdog.hpp)

```cpp
#pragma once

#include "watchdog/process_monitor.hpp"
#include "watchdog/state_persistence.hpp"
#include "watchdog/health_protocol.hpp"
#include <atomic>
#include <thread>

namespace watchdog {

struct WatchdogConfig {
    // 메인 프로세스 설정
    std::string main_executable;
    std::vector<std::string> main_arguments;
    std::string working_directory;
    
    // 하트비트 설정
    int heartbeat_interval_ms = 1000;       // 1초
    int heartbeat_timeout_ms = 5000;        // 5초 무응답 시 이상
    int max_missed_heartbeats = 3;          // 3회 연속 실패 시 재시작
    
    // 재시작 설정
    int max_restarts = 10;                  // 최대 재시작 횟수
    int restart_window_sec = 3600;          // 1시간 내 재시작 횟수 제한
    int restart_delay_ms = 5000;            // 재시작 전 대기
    bool restart_on_crash = true;
    bool restart_on_hang = true;            // 하트비트 타임아웃 시
    
    // 리소스 제한
    uint64_t max_memory_bytes = 4ULL * 1024 * 1024 * 1024;  // 4GB
    double max_cpu_percent = 90.0;
    int resource_check_interval_ms = 10000; // 10초
    
    // 상태 저장
    std::string state_directory = "./state";
    int state_save_interval_ms = 5000;      // 5초
    
    // 알림
    bool alert_on_restart = true;
    bool alert_on_resource_limit = true;
    std::string alert_webhook_url;          // Telegram/Slack 웹훅
};

class Watchdog {
public:
    explicit Watchdog(const WatchdogConfig& config);
    ~Watchdog();
    
    // 시작/중지
    void start();
    void stop();
    bool is_running() const { return running_.load(); }
    
    // 수동 제어
    void restart_main_process(const std::string& reason);
    void send_command(WatchdogCommand cmd);
    void trigger_state_save();
    
    // 상태 조회
    struct Status {
        bool main_process_running;
        int main_process_pid;
        uint64_t main_process_uptime_sec;
        int restart_count;
        std::chrono::system_clock::time_point last_heartbeat;
        std::chrono::system_clock::time_point last_restart;
        Heartbeat last_heartbeat_data;
    };
    Status get_status() const;
    
    // 콜백
    using RestartCallback = std::function<void(int old_pid, int new_pid, const std::string& reason)>;
    using AlertCallback = std::function<void(const std::string& level, const std::string& message)>;
    
    void on_restart(RestartCallback cb) { on_restart_ = std::move(cb); }
    void on_alert(AlertCallback cb) { on_alert_ = std::move(cb); }
    
private:
    void run_loop();
    void monitor_heartbeat();
    void monitor_resources();
    void handle_heartbeat(const Heartbeat& hb);
    void do_restart(const std::string& reason);
    void send_alert(const std::string& level, const std::string& message);
    
    bool wait_for_graceful_shutdown(int timeout_sec);
    void recover_state_after_restart();
    
private:
    WatchdogConfig config_;
    ProcessMonitor process_monitor_;
    StatePersistence state_persistence_;
    
    int main_pid_ = -1;
    std::atomic<bool> running_{false};
    std::jthread monitor_thread_;
    
    // 하트비트 추적
    std::chrono::steady_clock::time_point last_heartbeat_time_;
    int missed_heartbeat_count_ = 0;
    
    // 재시작 추적
    int restart_count_ = 0;
    std::chrono::steady_clock::time_point window_start_;
    
    // 콜백
    RestartCallback on_restart_;
    AlertCallback on_alert_;
    
    // IPC
    class IpcServer;
    std::unique_ptr<IpcServer> ipc_server_;
};

}  // namespace watchdog
```

### 5. 메인 프로세스 측 클라이언트 (watchdog_client.hpp)

```cpp
// include/arbitrage/infra/watchdog_client.hpp

#pragma once

#include <functional>
#include <atomic>
#include <thread>

namespace arbitrage {

// 메인 프로세스에서 사용하는 워치독 클라이언트
class WatchdogClient {
public:
    WatchdogClient();
    ~WatchdogClient();
    
    // 연결
    bool connect(const std::string& socket_path = "");
    void disconnect();
    bool is_connected() const;
    
    // 하트비트 자동 전송 시작
    void start_heartbeat(int interval_ms = 1000);
    void stop_heartbeat();
    
    // 상태 업데이트 (하트비트에 포함)
    void update_status(
        uint32_t active_connections,
        uint32_t pending_orders,
        uint8_t component_status,
        uint32_t error_count = 0
    );
    
    // 명령 수신 콜백
    using CommandCallback = std::function<void(WatchdogCommand cmd, const std::string& payload)>;
    void on_command(CommandCallback cb) { on_command_ = std::move(cb); }
    
    // 수동 상태 저장 요청 (크래시 예감 시)
    void request_state_save();
    
    // 워치독 없이 실행 중인지 확인
    bool is_standalone() const { return standalone_; }
    
private:
    void heartbeat_loop();
    void receive_loop();
    
private:
    std::atomic<bool> connected_{false};
    std::atomic<bool> standalone_{true};    // 워치독 없이 실행 시 true
    
    std::jthread heartbeat_thread_;
    std::jthread receive_thread_;
    
    // 현재 상태
    std::atomic<uint32_t> active_connections_{0};
    std::atomic<uint32_t> pending_orders_{0};
    std::atomic<uint8_t> component_status_{0};
    std::atomic<uint32_t> error_count_{0};
    std::atomic<uint64_t> sequence_{0};
    
    CommandCallback on_command_;
    
    // IPC 클라이언트
    class IpcClient;
    std::unique_ptr<IpcClient> ipc_client_;
};

// 메인 프로세스 통합 예시
/*
int main() {
    WatchdogClient watchdog;
    
    // 워치독 연결 시도 (없어도 동작)
    if (watchdog.connect()) {
        watchdog.start_heartbeat(1000);
        
        watchdog.on_command([](auto cmd, auto payload) {
            switch (cmd) {
                case WatchdogCommand::Shutdown:
                    graceful_shutdown();
                    break;
                case WatchdogCommand::SaveState:
                    save_state_immediately();
                    break;
                case WatchdogCommand::KillSwitch:
                    activate_kill_switch(payload);
                    break;
            }
        });
    }
    
    // 메인 루프에서 상태 업데이트
    while (running) {
        watchdog.update_status(
            websocket_manager.connection_count(),
            order_manager.pending_count(),
            get_component_status_flags(),
            error_counter.get()
        );
        
        // ... 메인 로직
    }
}
*/

}  // namespace arbitrage
```

---

## 📊 동작 흐름

```
┌─────────────────────────────────────────────────────────────┐
│                     시작 시퀀스                              │
├─────────────────────────────────────────────────────────────┤
│  1. 워치독 시작                                             │
│  2. 이전 상태 파일 확인 (크래시 복구?)                        │
│  3. 메인 프로세스 시작                                       │
│  4. IPC 연결 대기                                           │
│  5. 하트비트 모니터링 시작                                   │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                     정상 운영                               │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   Watchdog                          Main Process            │
│      │                                   │                  │
│      │◄─────── Heartbeat (1초) ──────────│                  │
│      │         {seq, status, mem, cpu}   │                  │
│      │                                   │                  │
│      │──── Command (필요시) ────────────►│                  │
│      │     {SaveState}                   │                  │
│      │                                   │                  │
│      │◄─────── Heartbeat ────────────────│                  │
│      │                                   │                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                     장애 감지 및 복구                        │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  [하트비트 타임아웃]                                         │
│  1. 5초 무응답 감지                                         │
│  2. HealthCheck 명령 전송                                   │
│  3. 3회 연속 실패 → 재시작 결정                             │
│                                                             │
│  [크래시 감지]                                              │
│  1. 프로세스 종료 감지 (exit code != 0)                     │
│  2. 코어 덤프 저장 (선택)                                   │
│  3. 즉시 재시작                                             │
│                                                             │
│  [리소스 초과]                                              │
│  1. 메모리 > 4GB 또는 CPU > 90%                            │
│  2. 경고 알림                                               │
│  3. 지속 시 정상 종료 요청 → 재시작                         │
│                                                             │
│  [복구 절차]                                                │
│  1. 이전 상태 파일 로드                                     │
│  2. 메인 프로세스 재시작                                    │
│  3. 복구 명령 전송 (열린 포지션, 대기 주문)                  │
│  4. 알림 발송                                               │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 📝 설정 파일 (watchdog.yaml)

```yaml
watchdog:
  # 메인 프로세스
  main_process:
    executable: "./arbitrage"
    arguments: ["--config", "./config/config.yaml"]
    working_directory: "./"
    
  # 하트비트
  heartbeat:
    interval_ms: 1000
    timeout_ms: 5000
    max_missed: 3
    
  # 재시작 정책
  restart:
    enabled: true
    on_crash: true
    on_hang: true
    max_restarts: 10
    window_sec: 3600              # 1시간 내 최대 횟수
    delay_ms: 5000                # 재시작 전 대기
    
  # 리소스 제한
  resources:
    check_interval_ms: 10000
    max_memory_gb: 4
    max_cpu_percent: 90
    action_on_exceed: restart     # warn / restart / kill
    
  # 상태 저장
  state:
    directory: "./state"
    save_interval_ms: 5000
    keep_snapshots: 100
    
  # 알림
  alerts:
    enabled: true
    on_restart: true
    on_crash: true
    on_resource_limit: true
    telegram:
      enabled: true
      bot_token: "${TELEGRAM_BOT_TOKEN}"
      chat_id: "${TELEGRAM_CHAT_ID}"
    webhook:
      enabled: false
      url: "https://hooks.slack.com/..."
      
  # 로깅
  logging:
    file: "./logs/watchdog.log"
    level: info
    max_size_mb: 100
    max_files: 10
```

---

## 🔗 의존성

```
TASK_24: Graceful Shutdown (정상 종료 처리)
TASK_25: Health Check (상태 정보 제공)
TASK_28: Alert System (알림 발송)
```

---

## ⚠️ 주의사항

```
1. 워치독 자체의 안정성
   - 워치독은 최대한 단순하게 유지
   - 복잡한 로직 금지, 메모리 할당 최소화
   - 워치독 크래시 시 systemd/서비스 매니저가 재시작

2. 상태 저장 타이밍
   - 너무 자주: I/O 부하
   - 너무 드물게: 데이터 손실
   - 권장: 5초 또는 중요 이벤트 후 즉시

3. IPC 선택
   - Unix Socket: 안정적, 범용
   - Shared Memory: 더 빠름, 구현 복잡
   - 네트워크 소켓: 원격 모니터링 가능하나 보안 주의

4. 재시작 폭주 방지
   - 설정 오류로 즉시 크래시 → 무한 재시작
   - window + max_restarts로 제한
   - 초과 시 알림 후 대기

5. 포지션 복구
   - 재시작 후 거래소 API로 실제 상태 확인 필수
   - 저장된 상태와 불일치 시 알림
```

---

## ✅ 완료 조건

```
□ 프로세스 모니터
  □ 시작/종료/강제종료
  □ 상태 조회 (메모리, CPU)
  □ Linux/Windows 지원

□ 하트비트 모니터링
  □ IPC 통신 (Unix Socket / Named Pipe)
  □ 타임아웃 감지
  □ 연속 실패 카운트

□ 상태 영속화
  □ 주기적 저장
  □ 크래시 복구
  □ 스냅샷 관리

□ 재시작 관리
  □ 자동 재시작
  □ 폭주 방지
  □ 복구 절차

□ 알림
  □ 재시작 알림
  □ 리소스 초과 알림
  □ Telegram/Webhook 연동

□ 메인 프로세스 클라이언트
  □ 하트비트 전송
  □ 명령 수신
  □ 워치독 없이도 동작

□ 단위/통합 테스트
```

---

## 📎 실행 방법

```bash
# 1. 워치독으로 실행 (권장)
./watchdog --config ./config/watchdog.yaml

# 2. 직접 실행 (개발/디버깅 시)
./arbitrage --config ./config/config.yaml

# 3. systemd 서비스로 등록 (Linux)
# /etc/systemd/system/arbitrage-watchdog.service
[Unit]
Description=Arbitrage Watchdog
After=network.target

[Service]
Type=simple
ExecStart=/opt/arbitrage/watchdog --config /opt/arbitrage/config/watchdog.yaml
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

---

## 📎 다음 태스크

완료 후: TASK_30_cli_tool.md (Phase 6 모니터링 시작)
