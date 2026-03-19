# TASK_42: Unix Domain Socket IPC

> Phase 3 기반

---

## 📌 목표

Cold Path 프로세스 간 통신을 위한 Unix Domain Socket 라이브러리

---

## 🔧 설계

### 프로토콜

```
[4-byte length][1-byte type][payload]
```

- Length: payload 크기 (big-endian)
- Type: 메시지 타입 enum
- Payload: POD struct 직렬화

### API

```cpp
// 서버
class UnixSocketServer {
    UnixSocketServer(const std::string& socket_path);
    void start();  // epoll 기반 accept + read
    void stop();
    void broadcast(uint8_t type, const void* data, size_t len);
    void on_message(MessageCallback cb);
};

// 클라이언트
class UnixSocketClient {
    bool connect(const std::string& socket_path);
    bool send(uint8_t type, const void* data, size_t len);
    void on_message(MessageCallback cb);
    void start_recv();  // 수신 스레드
};
```

### 소켓 경로

| 소켓 | 경로 | 방향 |
|------|------|------|
| Risk | `/tmp/kimchi_risk.sock` | Engine ↔ RiskManager |
| Monitor | `/tmp/kimchi_monitor.sock` | Engine ↔ Monitor |

---

## 📁 생성 파일

| 파일 | 내용 |
|------|------|
| `include/arbitrage/ipc/unix_socket.hpp` | Server/Client |
| `include/arbitrage/ipc/ipc_protocol.hpp` | 메시지 프레이밍 |
| `src/ipc/unix_socket.cpp` | 구현 |

## 📎 의존성: 없음 (Phase 2와 병렬 가능)
## ⏱️ 예상: 1.5일
