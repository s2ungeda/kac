# TASK_37: Feeder Process Base Class

> Phase 2

---

## 📌 목표

거래소별 Feed Handler 프로세스의 공통 기반 클래스

---

## 🔧 설계

### FeederProcess 역할

```
WebSocket 연결 → 시세 수신 → 파싱 → Ticker 추출 → SHM Queue write
```

### CLI 인터페이스

```bash
./upbit-feeder --exchange upbit \
               --shm /kimchi_feed_upbit \
               --capacity 4096 \
               --config config/config.yaml \
               --symbol KRW-XRP
```

### 클래스 구조

```cpp
class FeederProcess {
public:
    FeederProcess(Exchange exchange, const FeederConfig& config);
    void run();   // 블로킹 — WebSocket 이벤트 루프
    void stop();  // SIGTERM 핸들러에서 호출

private:
    void on_ticker(const Ticker& ticker);  // → SHM push
    ShmSegment shm_segment_;
    ShmSPSCQueue<Ticker> shm_queue_;
    std::shared_ptr<WebSocketClientBase> ws_client_;
};
```

### WebSocket 콜백

```cpp
ws_client_->on_event([this](const WebSocketEvent& evt) {
    if (evt.is_ticker() || evt.is_trade()) {
        shm_queue_.push(evt.ticker());  // Ticker는 POD → SHM 안전
    }
});
```

---

## 📁 생성 파일

| 파일 | 내용 |
|------|------|
| `include/arbitrage/feeder/feeder_process.hpp` | FeederProcess 클래스 |
| `src/feeder/feeder_process.cpp` | 구현 |

## 📎 의존성: TASK_36
## ⏱️ 예상: 1일
