# TASK 30: CLI 도구 (C++)

## 🎯 목표
시스템 관리 및 디버깅용 명령줄 도구

---

## 📁 생성할 파일

```
tools/cli/
├── main.cpp
├── commands.hpp
└── commands.cpp
```

---

## 📝 핵심 구현

```cpp
// CLI 명령어
class CLI {
public:
    CLI(const std::string& server_addr, int port);
    
    // 명령어
    void status();              // 시스템 상태
    void premium();             // 김프 매트릭스
    void balance();             // 잔고 조회
    void order(const OrderRequest& req);  // 수동 주문
    void cancel(const std::string& id);   // 주문 취소
    void history(int count);    // 거래 내역
    void config(const std::string& key, const std::string& value);  // 설정
    void kill();                // 킬스위치 활성화
    void resume();              // 킬스위치 해제
    
private:
    void send_command(const std::string& cmd, const json& params);
    json receive_response();
    
    int sock_fd_{-1};
};

// 메인
int main(int argc, char* argv[]) {
    CLI cli("localhost", 9800);
    
    if (argc < 2) {
        print_usage();
        return 1;
    }
    
    std::string cmd = argv[1];
    
    if (cmd == "status") {
        cli.status();
    } else if (cmd == "premium") {
        cli.premium();
    }
    // ...
    
    return 0;
}
```

---

## ✅ 완료 조건

```
□ TCP 클라이언트
□ 상태 조회
□ 수동 주문
□ 설정 변경
□ 킬스위치
```

---

## 📎 다음 태스크

완료 후: TASK_31_metrics_monitoring.md
