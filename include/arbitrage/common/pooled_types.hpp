#pragma once

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/memory_pool.hpp"

namespace arbitrage {

/**
 * 풀링 대상 타입들의 글로벌 풀 접근자
 *
 * 사용 예:
 *   // Ticker 객체 생성
 *   Ticker* t = ticker_pool().create();
 *   t->exchange = Exchange::Upbit;
 *   t->price = 50000.0;
 *
 *   // 처리 후 반환
 *   ticker_pool().destroy(t);
 */

// ============================================================================
// 풀 크기 설정
// ============================================================================

constexpr size_t TICKER_POOL_SIZE = 4096;      // 시세 데이터 (고빈도)
constexpr size_t ORDERBOOK_POOL_SIZE = 1024;   // 호가창 (중빈도)
constexpr size_t ORDER_POOL_SIZE = 256;        // 주문 (저빈도)
constexpr size_t BALANCE_POOL_SIZE = 64;       // 잔고 (저빈도)

// ============================================================================
// 글로벌 풀 접근자
// ============================================================================

/**
 * Ticker 객체 풀
 * 초당 수십~수백 개의 시세 데이터 처리
 */
inline ObjectPool<Ticker, TICKER_POOL_SIZE>& ticker_pool() {
    static ObjectPool<Ticker, TICKER_POOL_SIZE> pool;
    return pool;
}

/**
 * OrderBook 객체 풀
 * 호가창 데이터 (std::vector 포함으로 크기가 가변적)
 */
inline ObjectPool<OrderBook, ORDERBOOK_POOL_SIZE>& orderbook_pool() {
    static ObjectPool<OrderBook, ORDERBOOK_POOL_SIZE> pool;
    return pool;
}

/**
 * OrderRequest 객체 풀
 * 주문 요청
 */
inline ObjectPool<OrderRequest, ORDER_POOL_SIZE>& order_request_pool() {
    static ObjectPool<OrderRequest, ORDER_POOL_SIZE> pool;
    return pool;
}

/**
 * OrderResult 객체 풀
 * 주문 결과
 */
inline ObjectPool<OrderResult, ORDER_POOL_SIZE>& order_result_pool() {
    static ObjectPool<OrderResult, ORDER_POOL_SIZE> pool;
    return pool;
}

/**
 * Balance 객체 풀
 * 잔고 정보
 */
inline ObjectPool<Balance, BALANCE_POOL_SIZE>& balance_pool() {
    static ObjectPool<Balance, BALANCE_POOL_SIZE> pool;
    return pool;
}

// ============================================================================
// RAII 래퍼 (자동 반환)
// ============================================================================

/**
 * 풀 객체 자동 반환 래퍼
 *
 * 사용 예:
 *   {
 *       auto ticker = PooledPtr<Ticker>(ticker_pool());
 *       ticker->price = 50000.0;
 *   }  // 자동으로 풀에 반환
 */
template <typename T, size_t PoolSize = 1024>
class PooledPtr {
public:
    explicit PooledPtr(ObjectPool<T, PoolSize>& pool)
        : pool_(&pool)
        , ptr_(pool.create())
    {}

    template <typename... Args>
    PooledPtr(ObjectPool<T, PoolSize>& pool, Args&&... args)
        : pool_(&pool)
        , ptr_(pool.create(std::forward<Args>(args)...))
    {}

    ~PooledPtr() {
        if (ptr_) {
            pool_->destroy(ptr_);
        }
    }

    // 이동만 가능
    PooledPtr(PooledPtr&& other) noexcept
        : pool_(other.pool_)
        , ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }

    PooledPtr& operator=(PooledPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_) {
                pool_->destroy(ptr_);
            }
            pool_ = other.pool_;
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // 복사 금지
    PooledPtr(const PooledPtr&) = delete;
    PooledPtr& operator=(const PooledPtr&) = delete;

    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    /**
     * 소유권 해제 (풀에 반환하지 않음)
     */
    T* release() noexcept {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }

private:
    ObjectPool<T, PoolSize>* pool_;
    T* ptr_;
};

// 편의를 위한 타입 별칭
using PooledTicker = PooledPtr<Ticker, TICKER_POOL_SIZE>;
using PooledOrderBook = PooledPtr<OrderBook, ORDERBOOK_POOL_SIZE>;
using PooledOrderRequest = PooledPtr<OrderRequest, ORDER_POOL_SIZE>;
using PooledOrderResult = PooledPtr<OrderResult, ORDER_POOL_SIZE>;
using PooledBalance = PooledPtr<Balance, BALANCE_POOL_SIZE>;

// ============================================================================
// 풀 통계
// ============================================================================

/**
 * 모든 풀의 통계 정보
 */
struct PoolStats {
    size_t ticker_available;
    size_t ticker_exhausted;      // 풀 소진 횟수
    size_t orderbook_available;
    size_t orderbook_exhausted;
    size_t order_available;
    size_t order_exhausted;
    size_t balance_available;
    size_t balance_exhausted;
};

/**
 * 현재 풀 통계 조회
 */
inline PoolStats get_pool_stats() {
    return PoolStats{
        .ticker_available = ticker_pool().available(),
        .ticker_exhausted = ticker_pool().exhausted_count(),
        .orderbook_available = orderbook_pool().available(),
        .orderbook_exhausted = orderbook_pool().exhausted_count(),
        .order_available = order_request_pool().available(),
        .order_exhausted = order_request_pool().exhausted_count(),
        .balance_available = balance_pool().available(),
        .balance_exhausted = balance_pool().exhausted_count()
    };
}

}  // namespace arbitrage
