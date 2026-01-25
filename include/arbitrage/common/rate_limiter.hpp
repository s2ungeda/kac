#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/spin_wait.hpp"

#include <atomic>
#include <chrono>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <thread>

namespace arbitrage {

/**
 * API 타입 (Rate Limit 구분용)
 */
enum class ApiType : uint8_t {
    Order,      // 주문 API (엄격한 제한)
    Query,      // 조회 API (느슨한 제한)
    WebSocket   // WebSocket 메시지 (별도 제한)
};

/**
 * Token Bucket Rate Limiter
 *
 * 특징:
 * - Lock-Free 토큰 획득
 * - 버스트 허용 (burst까지 즉시 처리)
 * - 자동 토큰 리필
 *
 * 사용 예:
 *   TokenBucketRateLimiter limiter(10.0, 20);  // 초당 10회, 버스트 20
 *   if (limiter.try_acquire()) {
 *       // API 호출
 *   }
 */
class TokenBucketRateLimiter {
public:
    /**
     * @param rate 초당 토큰 생성 속도
     * @param burst 최대 토큰 수 (버스트 크기)
     */
    TokenBucketRateLimiter(double rate, size_t burst)
        : rate_(rate)
        , burst_(static_cast<double>(burst))
        , tokens_(static_cast<double>(burst))
        , last_refill_(std::chrono::steady_clock::now())
    {}

    /**
     * 토큰 획득 시도 (논블로킹)
     * @param count 필요한 토큰 수
     * @return 성공 시 true
     */
    bool try_acquire(size_t count = 1) noexcept {
        refill();

        double needed = static_cast<double>(count);
        double current = tokens_.load(std::memory_order_relaxed);

        while (current >= needed) {
            if (tokens_.compare_exchange_weak(
                    current, current - needed,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    /**
     * 토큰 획득 (블로킹)
     * @param count 필요한 토큰 수
     */
    void acquire(size_t count = 1) noexcept {
        AdaptiveSpinWait waiter;
        while (!try_acquire(count)) {
            waiter.wait();
        }
    }

    /**
     * 토큰 획득 대기 (타임아웃)
     * @param count 필요한 토큰 수
     * @param timeout 최대 대기 시간
     * @return 성공 시 true, 타임아웃 시 false
     */
    template <typename Rep, typename Period>
    bool acquire_for(size_t count,
                     std::chrono::duration<Rep, Period> timeout) noexcept {
        auto start = std::chrono::steady_clock::now();
        AdaptiveSpinWait waiter;

        while (!try_acquire(count)) {
            if (std::chrono::steady_clock::now() - start >= timeout) {
                return false;
            }
            waiter.wait();
        }
        return true;
    }

    /**
     * 현재 토큰 수 (대략적)
     */
    [[nodiscard]] double tokens() const noexcept {
        return tokens_.load(std::memory_order_relaxed);
    }

    /**
     * 토큰 리필 속도 (초당)
     */
    [[nodiscard]] double rate() const noexcept {
        return rate_;
    }

    /**
     * 버스트 크기
     */
    [[nodiscard]] double burst() const noexcept {
        return burst_;
    }

private:
    void refill() noexcept {
        auto now = std::chrono::steady_clock::now();

        // 마지막 리필 시간 획득
        auto last = last_refill_.load(std::memory_order_relaxed);
        auto elapsed = std::chrono::duration<double>(now - last).count();

        if (elapsed < 0.001) {
            return;  // 1ms 미만이면 스킵
        }

        // CAS로 리필 시간 업데이트
        if (!last_refill_.compare_exchange_weak(
                last, now,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return;  // 다른 스레드가 리필 중
        }

        // 토큰 추가
        double add = elapsed * rate_;
        double current = tokens_.load(std::memory_order_relaxed);
        double new_tokens = std::min(current + add, burst_);

        while (!tokens_.compare_exchange_weak(
                   current, new_tokens,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
            new_tokens = std::min(current + add, burst_);
        }
    }

    const double rate_;
    const double burst_;
    std::atomic<double> tokens_;
    std::atomic<std::chrono::steady_clock::time_point> last_refill_;
};


/**
 * 거래소별 Rate Limit 설정
 */
struct ExchangeRateLimits {
    double order_rate;      // 주문 API 초당 호출 수
    size_t order_burst;     // 주문 API 버스트
    double query_rate;      // 조회 API 초당 호출 수
    size_t query_burst;     // 조회 API 버스트
};

/**
 * 기본 Rate Limit 설정
 */
inline ExchangeRateLimits get_default_limits(Exchange ex) {
    switch (ex) {
        case Exchange::Upbit:
            return {8.0, 10, 30.0, 50};      // 주문: 8/s, 조회: 30/s
        case Exchange::Bithumb:
            return {10.0, 15, 20.0, 30};     // 주문: 10/s, 조회: 20/s
        case Exchange::Binance:
            return {20.0, 30, 100.0, 150};   // 주문: 20/s, 조회: 100/s (1200/min)
        case Exchange::MEXC:
            return {20.0, 30, 50.0, 80};     // 주문: 20/s, 조회: 50/s
        default:
            return {10.0, 15, 30.0, 50};
    }
}


/**
 * Rate Limit Manager
 *
 * 거래소/API별 Rate Limiter 통합 관리
 *
 * 사용 예:
 *   rate_limits().acquire(Exchange::Upbit, ApiType::Order);
 *   // API 호출
 */
class RateLimitManager {
public:
    RateLimitManager() {
        // 거래소별 Limiter 초기화
        for (int i = 0; i < static_cast<int>(Exchange::Count); ++i) {
            auto ex = static_cast<Exchange>(i);
            auto limits = get_default_limits(ex);

            order_limiters_[ex] = std::make_unique<TokenBucketRateLimiter>(
                limits.order_rate, limits.order_burst);
            query_limiters_[ex] = std::make_unique<TokenBucketRateLimiter>(
                limits.query_rate, limits.query_burst);
        }
    }

    /**
     * 토큰 획득 (블로킹)
     */
    void acquire(Exchange ex, ApiType type, size_t count = 1) {
        get_limiter(ex, type)->acquire(count);
    }

    /**
     * 토큰 획득 시도 (논블로킹)
     */
    bool try_acquire(Exchange ex, ApiType type, size_t count = 1) {
        return get_limiter(ex, type)->try_acquire(count);
    }

    /**
     * 토큰 획득 (타임아웃)
     */
    template <typename Rep, typename Period>
    bool acquire_for(Exchange ex, ApiType type, size_t count,
                     std::chrono::duration<Rep, Period> timeout) {
        return get_limiter(ex, type)->acquire_for(count, timeout);
    }

    /**
     * 현재 토큰 수
     */
    [[nodiscard]] double tokens(Exchange ex, ApiType type) const {
        return get_limiter(ex, type)->tokens();
    }

    /**
     * Rate Limiter 조회
     */
    TokenBucketRateLimiter* get_limiter(Exchange ex, ApiType type) const {
        if (type == ApiType::Order) {
            auto it = order_limiters_.find(ex);
            return it != order_limiters_.end() ? it->second.get() : nullptr;
        } else {
            auto it = query_limiters_.find(ex);
            return it != query_limiters_.end() ? it->second.get() : nullptr;
        }
    }

    /**
     * Rate Limit 설정 변경
     */
    void set_limits(Exchange ex, const ExchangeRateLimits& limits) {
        order_limiters_[ex] = std::make_unique<TokenBucketRateLimiter>(
            limits.order_rate, limits.order_burst);
        query_limiters_[ex] = std::make_unique<TokenBucketRateLimiter>(
            limits.query_rate, limits.query_burst);
    }

private:
    std::unordered_map<Exchange, std::unique_ptr<TokenBucketRateLimiter>> order_limiters_;
    std::unordered_map<Exchange, std::unique_ptr<TokenBucketRateLimiter>> query_limiters_;
};


/**
 * 글로벌 Rate Limit Manager
 */
inline RateLimitManager& rate_limits() {
    static RateLimitManager instance;
    return instance;
}


/**
 * RAII Rate Limit Guard
 *
 * 스코프 진입 시 자동으로 토큰 획득
 */
class RateLimitGuard {
public:
    RateLimitGuard(Exchange ex, ApiType type, size_t count = 1)
        : acquired_(true)
    {
        rate_limits().acquire(ex, type, count);
    }

    // try 버전
    static std::optional<RateLimitGuard> try_guard(Exchange ex, ApiType type, size_t count = 1) {
        if (rate_limits().try_acquire(ex, type, count)) {
            RateLimitGuard guard;
            guard.acquired_ = true;
            return guard;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool acquired() const noexcept {
        return acquired_;
    }

private:
    RateLimitGuard() : acquired_(false) {}
    bool acquired_;
};

}  // namespace arbitrage
