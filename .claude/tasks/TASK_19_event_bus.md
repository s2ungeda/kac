# TASK 23: 이벤트 버스 (C++)

## 🎯 목표
컴포넌트 간 느슨한 결합을 위한 Pub/Sub 시스템

---

## 📁 생성할 파일

```
include/arbitrage/infra/
├── event_bus.hpp
└── events.hpp
src/infra/
└── event_bus.cpp
```

---

## 📝 핵심 구현

### 1. events.hpp

```cpp
#pragma once

#include "arbitrage/common/types.hpp"
#include <variant>
#include <chrono>

namespace arbitrage::events {

// 기본 이벤트
struct EventBase {
    std::string id;
    std::chrono::system_clock::time_point timestamp;
    EventBase();
};

// 시세 이벤트
struct TickerReceived : EventBase {
    Ticker ticker;
};

// 김프 이벤트
struct PremiumUpdated : EventBase {
    double premium_pct;
    Exchange buy_exchange;
    Exchange sell_exchange;
};

struct OpportunityDetected : EventBase {
    double premium_pct;
    Exchange buy_exchange;
    Exchange sell_exchange;
    double recommended_qty;
};

// 주문 이벤트
struct OrderSubmitted : EventBase {
    std::string order_id;
    Exchange exchange;
};

struct OrderFilled : EventBase {
    std::string order_id;
    double filled_qty;
    double avg_price;
};

struct OrderFailed : EventBase {
    std::string order_id;
    std::string error;
};

// 시스템 이벤트
struct ExchangeConnected : EventBase {
    Exchange exchange;
};

struct ExchangeDisconnected : EventBase {
    Exchange exchange;
    std::string reason;
};

struct KillSwitchActivated : EventBase {
    std::string reason;
};

// 이벤트 타입
using Event = std::variant<
    TickerReceived,
    PremiumUpdated,
    OpportunityDetected,
    OrderSubmitted,
    OrderFilled,
    OrderFailed,
    ExchangeConnected,
    ExchangeDisconnected,
    KillSwitchActivated
>;

}  // namespace arbitrage::events
```

### 2. event_bus.hpp

```cpp
#pragma once

#include "arbitrage/infra/events.hpp"
#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <queue>
#include <thread>
#include <condition_variable>

namespace arbitrage {

// 구독 토큰
class SubscriptionToken {
public:
    explicit SubscriptionToken(uint64_t id) : id_(id) {}
    uint64_t id() const { return id_; }
    bool operator==(const SubscriptionToken& other) const { return id_ == other.id_; }
private:
    uint64_t id_;
};

// 핸들러 타입
template<typename E>
using EventHandler = std::function<void(const E&)>;
using GenericHandler = std::function<void(const events::Event&)>;

class EventBus : public std::enable_shared_from_this<EventBus> {
public:
    static std::shared_ptr<EventBus> instance();
    
    EventBus();
    ~EventBus();
    
    // 비동기 모드 (워커 스레드)
    void start_async(size_t worker_count = 1);
    void stop();
    
    // 이벤트 발행
    template<typename E>
    void publish(const E& event);
    
    // 특정 이벤트 구독
    template<typename E>
    SubscriptionToken subscribe(EventHandler<E> handler);
    
    // 모든 이벤트 구독
    SubscriptionToken subscribe_all(GenericHandler handler);
    
    // 구독 해제
    void unsubscribe(SubscriptionToken token);
    
private:
    void dispatch(const events::Event& event);
    void worker_thread();
    
private:
    mutable std::shared_mutex handlers_mutex_;
    std::unordered_map<uint64_t, GenericHandler> handlers_;
    std::atomic<uint64_t> next_token_id_{1};
    
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<events::Event> event_queue_;
    
    std::vector<std::jthread> workers_;
    std::atomic<bool> running_{false};
};

// 템플릿 구현
template<typename E>
void EventBus::publish(const E& event) {
    events::Event generic_event = event;
    
    if (running_) {
        std::unique_lock lock(queue_mutex_);
        event_queue_.push(generic_event);
        lock.unlock();
        queue_cv_.notify_one();
    } else {
        dispatch(generic_event);
    }
}

template<typename E>
SubscriptionToken EventBus::subscribe(EventHandler<E> handler) {
    auto token = SubscriptionToken(next_token_id_++);
    
    GenericHandler generic = [handler](const events::Event& event) {
        if (auto* e = std::get_if<E>(&event)) {
            handler(*e);
        }
    };
    
    std::unique_lock lock(handlers_mutex_);
    handlers_[token.id()] = std::move(generic);
    
    return token;
}

}  // namespace arbitrage
```

---

## ✅ 완료 조건

```
□ std::variant 이벤트 타입
□ 타입 안전 구독
□ 비동기 처리 (워커)
□ 구독 해제
□ 스레드 안전
```

---

## 📎 다음 태스크

완료 후: TASK_24_graceful_shutdown.md
