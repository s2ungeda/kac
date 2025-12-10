# TASK 27: TCP 서버 (C++)

## 🎯 목표
Delphi 클라이언트 연동을 위한 TCP 서버

---

## 📁 생성할 파일

```
include/arbitrage/infra/
└── tcp_server.hpp
src/infra/
└── tcp_server.cpp
```

---

## 📝 핵심 구현

```cpp
class TcpServer {
public:
    TcpServer(const std::string& bind_addr, int port);
    ~TcpServer();
    
    // 서버 시작/중지
    Result<void> start();
    void stop();
    
    // 이벤트 루프
    void run();
    void poll();
    
    // 브로드캐스트
    void broadcast(const std::vector<uint8_t>& message);
    
    // 콜백
    using MessageCallback = std::function<void(int client_id, 
                                                const MessageHeader&,
                                                const msgpack::object&)>;
    void on_message(MessageCallback cb);
    
private:
    void accept_loop();
    void client_loop(int client_fd, int client_id);
    
    int server_fd_{-1};
    std::atomic<bool> running_{false};
    
    mutable std::mutex clients_mutex_;
    std::map<int, int> clients_;  // client_id -> fd
    
    std::string bind_addr_;
    int port_;
};
```

---

## ✅ 완료 조건

```
□ TCP 리슨
□ 다중 클라이언트
□ MessagePack 처리
□ 브로드캐스트
□ 인증
```

---

## 📎 다음 태스크

완료 후: TASK_28_alert_system.md
