/**
 * Event Bus Implementation (TASK_19)
 */

#include "arbitrage/infra/event_bus.hpp"
#include "arbitrage/common/logger.hpp"

namespace arbitrage {

namespace {
    auto logger() {
        static auto log = Logger::get("EventBus");
        return log;
    }
}

// =============================================================================
// SubscriptionGuard
// =============================================================================

SubscriptionGuard::SubscriptionGuard(std::shared_ptr<EventBus> bus, SubscriptionToken token)
    : bus_(bus)
    , token_(token)
{}

SubscriptionGuard::~SubscriptionGuard() {
    if (token_.valid()) {
        if (auto bus = bus_.lock()) {
            bus->unsubscribe(token_);
        }
    }
}

SubscriptionGuard::SubscriptionGuard(SubscriptionGuard&& other) noexcept
    : bus_(std::move(other.bus_))
    , token_(other.token_)
{
    other.token_ = SubscriptionToken();
}

SubscriptionGuard& SubscriptionGuard::operator=(SubscriptionGuard&& other) noexcept {
    if (this != &other) {
        // 기존 구독 해제
        if (token_.valid()) {
            if (auto bus = bus_.lock()) {
                bus->unsubscribe(token_);
            }
        }

        bus_ = std::move(other.bus_);
        token_ = other.token_;
        other.token_ = SubscriptionToken();
    }
    return *this;
}

void SubscriptionGuard::release() {
    token_ = SubscriptionToken();
}

// =============================================================================
// EventBus
// =============================================================================

std::shared_ptr<EventBus> EventBus::instance() {
    static auto inst = std::make_shared<EventBus>();
    return inst;
}

EventBus::EventBus() {
    logger()->debug("EventBus created");
}

EventBus::~EventBus() {
    stop();
    logger()->debug("EventBus destroyed");
}

void EventBus::start_async(size_t worker_count) {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        // 이미 실행 중
        return;
    }

    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this] { worker_thread(); });
    }

    logger()->info("EventBus started with {} workers", worker_count);
}

void EventBus::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        // 이미 중지됨
        return;
    }

    // 모든 워커 깨우기
    queue_wakeup_.store(true, std::memory_order_release);

    // 워커 종료 대기
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    // 남은 이벤트 처리 (동기)
    {
        SpinLockGuard lock(queue_mutex_);
        while (!event_queue_.empty()) {
            dispatch(event_queue_.front());
            event_queue_.pop();
        }
    }

    logger()->info("EventBus stopped");
}

SubscriptionToken EventBus::subscribe_all(GenericHandler handler) {
    auto token = SubscriptionToken(next_token_id_.fetch_add(1, std::memory_order_relaxed));

    {
        WriteGuard lock(handlers_mutex_);
        handlers_[token.id()] = std::move(handler);
    }

    return token;
}

SubscriptionGuard EventBus::subscribe_all_guarded(GenericHandler handler) {
    auto token = subscribe_all(std::move(handler));
    return SubscriptionGuard(shared_from_this(), token);
}

void EventBus::unsubscribe(SubscriptionToken token) {
    if (!token.valid()) {
        return;
    }

    WriteGuard lock(handlers_mutex_);
    handlers_.erase(token.id());
}

size_t EventBus::subscriber_count() const {
    ReadGuard lock(handlers_mutex_);
    return handlers_.size();
}

size_t EventBus::pending_event_count() const {
    SpinLockGuard lock(queue_mutex_);
    return event_queue_.size();
}

void EventBus::dispatch(const events::Event& event) {
    events_dispatched_.fetch_add(1, std::memory_order_relaxed);

    // 핸들러 복사 (락 최소화)
    std::vector<GenericHandler> handlers_copy;
    {
        ReadGuard lock(handlers_mutex_);
        handlers_copy.reserve(handlers_.size());
        for (const auto& [id, handler] : handlers_) {
            handlers_copy.push_back(handler);
        }
    }

    // 핸들러 호출 (락 외부)
    for (const auto& handler : handlers_copy) {
        try {
            handler(event);
        } catch (const std::exception& e) {
            logger()->error("EventBus handler error: {}", e.what());
        } catch (...) {
            logger()->error("EventBus handler unknown error");
        }
    }
}

void EventBus::worker_thread() {
    while (running_.load(std::memory_order_acquire)) {
        events::Event event;
        bool has_event = false;

        // 이벤트 대기 (SpinWait)
        SpinWait::until([this] {
            return queue_wakeup_.load(std::memory_order_acquire) ||
                   !running_.load(std::memory_order_acquire);
        });
        queue_wakeup_.store(false, std::memory_order_release);

        if (!running_.load(std::memory_order_acquire)) {
            // 종료 전 남은 이벤트 처리 시도
            SpinLockGuard lock(queue_mutex_);
            if (event_queue_.empty()) {
                break;
            }
            event = std::move(event_queue_.front());
            event_queue_.pop();
            has_event = true;
        } else {
            SpinLockGuard lock(queue_mutex_);
            if (!event_queue_.empty()) {
                event = std::move(event_queue_.front());
                event_queue_.pop();
                has_event = true;
            }
        }

        if (has_event) {
            dispatch(event);
        }
    }
}

}  // namespace arbitrage
