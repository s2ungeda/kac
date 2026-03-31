#pragma once

/**
 * Event Bus (TASK_19)
 *
 * 컴포넌트 간 느슨한 결합을 위한 Pub/Sub 시스템
 * - 타입 안전 이벤트 구독
 * - 비동기 처리 (워커 스레드)
 * - 구독 해제 지원
 */

#include "arbitrage/infra/events.hpp"
#include "arbitrage/common/spin_wait.hpp"

#include <functional>
#include <memory>
#include <unordered_map>
#include <queue>
#include <thread>
#include <atomic>
#include <vector>
#include <typeindex>

namespace arbitrage {

// =============================================================================
// 구독 토큰
// =============================================================================
class SubscriptionToken {
public:
    SubscriptionToken() : id_(0) {}
    explicit SubscriptionToken(uint64_t id) : id_(id) {}

    uint64_t id() const { return id_; }
    bool valid() const { return id_ != 0; }

    bool operator==(const SubscriptionToken& other) const { return id_ == other.id_; }
    bool operator!=(const SubscriptionToken& other) const { return id_ != other.id_; }

private:
    uint64_t id_;
};

// =============================================================================
// 핸들러 타입
// =============================================================================
template<typename E>
using EventHandler = std::function<void(const E&)>;
using GenericHandler = std::function<void(const events::Event&)>;

// =============================================================================
// RAII 구독 가드
// =============================================================================
class EventBus;  // Forward declaration

class SubscriptionGuard {
public:
    SubscriptionGuard() = default;
    SubscriptionGuard(std::shared_ptr<EventBus> bus, SubscriptionToken token);
    ~SubscriptionGuard();

    SubscriptionGuard(const SubscriptionGuard&) = delete;
    SubscriptionGuard& operator=(const SubscriptionGuard&) = delete;

    SubscriptionGuard(SubscriptionGuard&& other) noexcept;
    SubscriptionGuard& operator=(SubscriptionGuard&& other) noexcept;

    SubscriptionToken token() const { return token_; }
    void release();  // 자동 해제 비활성화

private:
    std::weak_ptr<EventBus> bus_;
    SubscriptionToken token_;
};

// =============================================================================
// EventBus
// =============================================================================
class EventBus : public std::enable_shared_from_this<EventBus> {
public:
    /**
     * 싱글톤 인스턴스
     */
    static std::shared_ptr<EventBus> instance();

    EventBus();
    ~EventBus();

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // =========================================================================
    // 비동기 모드 (워커 스레드)
    // =========================================================================

    /**
     * 비동기 모드 시작
     * @param worker_count 워커 스레드 수
     */
    void start_async(size_t worker_count = 1);

    /**
     * 비동기 모드 중지
     */
    void stop();

    /**
     * 실행 중인지 확인
     */
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // =========================================================================
    // 이벤트 발행
    // =========================================================================

    /**
     * 이벤트 발행
     * - 동기 모드: 즉시 dispatch
     * - 비동기 모드: 큐에 추가
     */
    template<typename E>
    void publish(const E& event);

    /**
     * 이벤트 발행 (이동)
     */
    template<typename E>
    void publish(E&& event);

    // =========================================================================
    // 이벤트 구독
    // =========================================================================

    /**
     * 특정 타입 이벤트 구독
     * @return 구독 토큰 (해제 시 사용)
     */
    template<typename E>
    SubscriptionToken subscribe(EventHandler<E> handler);

    /**
     * 특정 타입 이벤트 구독 (RAII 가드 반환)
     */
    template<typename E>
    SubscriptionGuard subscribe_guarded(EventHandler<E> handler);

    /**
     * 모든 이벤트 구독
     */
    SubscriptionToken subscribe_all(GenericHandler handler);

    /**
     * 모든 이벤트 구독 (RAII 가드 반환)
     */
    SubscriptionGuard subscribe_all_guarded(GenericHandler handler);

    /**
     * 구독 해제
     */
    void unsubscribe(SubscriptionToken token);

    // =========================================================================
    // 통계
    // =========================================================================

    /**
     * 구독자 수
     */
    size_t subscriber_count() const;

    /**
     * 대기 중인 이벤트 수
     */
    size_t pending_event_count() const;

    /**
     * 발행된 이벤트 총 수
     */
    uint64_t total_events_published() const {
        return events_published_.load(std::memory_order_relaxed);
    }

    /**
     * 처리된 이벤트 총 수
     */
    uint64_t total_events_dispatched() const {
        return events_dispatched_.load(std::memory_order_relaxed);
    }

private:
    /**
     * 이벤트 dispatch (모든 핸들러 호출)
     */
    void dispatch(const events::Event& event);

    /**
     * 워커 스레드 함수
     */
    void worker_thread();

private:
    // 핸들러 저장
    mutable RWSpinLock handlers_mutex_;
    std::unordered_map<uint64_t, GenericHandler> handlers_;
    std::atomic<uint64_t> next_token_id_{1};

    // 이벤트 큐 (비동기 모드)
    mutable SpinLock queue_mutex_;
    std::atomic<bool> queue_wakeup_{false};
    std::queue<events::Event> event_queue_;

    // 워커 스레드
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};

    // 통계
    std::atomic<uint64_t> events_published_{0};
    std::atomic<uint64_t> events_dispatched_{0};
};

// =============================================================================
// 템플릿 구현
// =============================================================================

template<typename E>
void EventBus::publish(const E& event) {
    static_assert(std::is_base_of_v<events::EventBase, E>,
                  "Event must derive from EventBase");

    events_published_.fetch_add(1, std::memory_order_relaxed);
    events::Event generic_event = event;

    if (running_.load(std::memory_order_acquire)) {
        // 비동기 모드: 큐에 추가
        {
            SpinLockGuard lock(queue_mutex_);
            event_queue_.push(std::move(generic_event));
        }
        queue_wakeup_.store(true, std::memory_order_release);
    } else {
        // 동기 모드: 즉시 dispatch
        dispatch(generic_event);
    }
}

template<typename E>
void EventBus::publish(E&& event) {
    static_assert(std::is_base_of_v<events::EventBase, std::decay_t<E>>,
                  "Event must derive from EventBase");

    events_published_.fetch_add(1, std::memory_order_relaxed);
    events::Event generic_event = std::forward<E>(event);

    if (running_.load(std::memory_order_acquire)) {
        {
            SpinLockGuard lock(queue_mutex_);
            event_queue_.push(std::move(generic_event));
        }
        queue_wakeup_.store(true, std::memory_order_release);
    } else {
        dispatch(generic_event);
    }
}

template<typename E>
SubscriptionToken EventBus::subscribe(EventHandler<E> handler) {
    static_assert(std::is_base_of_v<events::EventBase, E>,
                  "Event must derive from EventBase");

    auto token = SubscriptionToken(next_token_id_.fetch_add(1, std::memory_order_relaxed));

    // 타입 안전 래퍼
    GenericHandler generic = [handler = std::move(handler)](const events::Event& event) {
        if (auto* e = std::get_if<E>(&event)) {
            handler(*e);
        }
    };

    {
        WriteGuard lock(handlers_mutex_);
        handlers_[token.id()] = std::move(generic);
    }

    return token;
}

template<typename E>
SubscriptionGuard EventBus::subscribe_guarded(EventHandler<E> handler) {
    auto token = subscribe<E>(std::move(handler));
    return SubscriptionGuard(shared_from_this(), token);
}

}  // namespace arbitrage
