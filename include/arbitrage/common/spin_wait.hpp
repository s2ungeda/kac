#pragma once

#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>

// x86 PAUSE 명령어
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <emmintrin.h>  // _mm_pause
    #define CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
    #define CPU_PAUSE() std::this_thread::yield()
#endif

namespace arbitrage {

/**
 * SpinWait 유틸리티
 *
 * OS 스케줄러 우회로 나노초 단위 즉각 반응
 * sleep(1ms) 실제 대기: 2~15ms → SpinWait: ~100ns
 */
class SpinWait {
public:
    /**
     * 단순 스핀 (CPU 친화적)
     * _mm_pause() 명령으로 파이프라인 낭비 방지
     */
    static void spin() noexcept {
        CPU_PAUSE();
    }

    /**
     * N회 스핀
     */
    static void spin(uint32_t count) noexcept {
        for (uint32_t i = 0; i < count; ++i) {
            CPU_PAUSE();
        }
    }

    /**
     * 조건 만족할 때까지 대기
     * @param pred 조건 함수 (true 반환 시 종료)
     */
    template <typename Predicate>
    static void until(Predicate&& pred) noexcept {
        while (!pred()) {
            CPU_PAUSE();
        }
    }

    /**
     * 조건 만족할 때까지 대기 (타임아웃 포함)
     * @param pred 조건 함수
     * @param timeout 타임아웃
     * @return 조건 만족 시 true, 타임아웃 시 false
     */
    template <typename Predicate, typename Rep, typename Period>
    static bool until_for(Predicate&& pred,
                          std::chrono::duration<Rep, Period> timeout) noexcept {
        auto start = std::chrono::steady_clock::now();
        while (!pred()) {
            if (std::chrono::steady_clock::now() - start >= timeout) {
                return false;
            }
            CPU_PAUSE();
        }
        return true;
    }
};


/**
 * 적응형 스핀 대기
 *
 * 스핀 → yield → sleep 단계적 전환
 * 짧은 대기는 스핀으로, 긴 대기는 CPU 절약
 */
class AdaptiveSpinWait {
public:
    /**
     * 한 번 대기
     * 호출 횟수에 따라 자동으로 적응
     */
    void wait() noexcept {
        if (count_ < kSpinThreshold) {
            // Phase 1: 스핀 (0~10회)
            CPU_PAUSE();
        } else if (count_ < kYieldThreshold) {
            // Phase 2: Yield (10~20회)
            std::this_thread::yield();
        } else {
            // Phase 3: Sleep (20회 이상)
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        ++count_;
    }

    /**
     * 조건 만족할 때까지 대기
     */
    template <typename Predicate>
    void until(Predicate&& pred) noexcept {
        while (!pred()) {
            wait();
        }
    }

    /**
     * 카운터 리셋
     */
    void reset() noexcept {
        count_ = 0;
    }

    /**
     * 현재 대기 횟수
     */
    [[nodiscard]] uint32_t count() const noexcept {
        return count_;
    }

private:
    static constexpr uint32_t kSpinThreshold = 10;
    static constexpr uint32_t kYieldThreshold = 20;
    uint32_t count_{0};
};


/**
 * SpinLock (std::mutex 대체)
 *
 * 특징:
 * - Lock-Free CAS 기반
 * - 컨텍스트 스위칭 없음
 * - 짧은 임계 구역에 최적
 *
 * 주의: 긴 임계 구역에서는 std::mutex 사용 권장
 */
class SpinLock {
public:
    SpinLock() = default;

    // 복사/이동 금지
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    /**
     * 락 획득
     */
    void lock() noexcept {
        // TTAS (Test-and-Test-and-Set) 패턴
        while (true) {
            // 먼저 읽기로 확인 (캐시 친화적)
            if (!locked_.load(std::memory_order_relaxed)) {
                // 락 시도
                if (!locked_.exchange(true, std::memory_order_acquire)) {
                    return;
                }
            }
            // 스핀 대기
            CPU_PAUSE();
        }
    }

    /**
     * 락 획득 시도
     * @return 성공 시 true
     */
    [[nodiscard]] bool try_lock() noexcept {
        return !locked_.load(std::memory_order_relaxed) &&
               !locked_.exchange(true, std::memory_order_acquire);
    }

    /**
     * 락 해제
     */
    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
    }

    /**
     * 락 상태 확인
     */
    [[nodiscard]] bool is_locked() const noexcept {
        return locked_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> locked_{false};
};


/**
 * RAII 스타일 SpinLock Guard
 */
class SpinLockGuard {
public:
    explicit SpinLockGuard(SpinLock& lock) noexcept
        : lock_(lock)
    {
        lock_.lock();
    }

    ~SpinLockGuard() {
        lock_.unlock();
    }

    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    SpinLock& lock_;
};


/**
 * Read-Write SpinLock
 *
 * 다수의 Reader와 단일 Writer 지원
 */
class RWSpinLock {
public:
    RWSpinLock() = default;

    /**
     * 읽기 락 획득
     */
    void lock_shared() noexcept {
        while (true) {
            // Writer가 없을 때만 진입
            int32_t expected = state_.load(std::memory_order_relaxed);
            if (expected >= 0) {
                if (state_.compare_exchange_weak(
                        expected, expected + 1,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    return;
                }
            }
            CPU_PAUSE();
        }
    }

    /**
     * 읽기 락 해제
     */
    void unlock_shared() noexcept {
        state_.fetch_sub(1, std::memory_order_release);
    }

    /**
     * 쓰기 락 획득
     */
    void lock() noexcept {
        while (true) {
            int32_t expected = 0;
            if (state_.compare_exchange_weak(
                    expected, -1,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return;
            }
            CPU_PAUSE();
        }
    }

    /**
     * 쓰기 락 해제
     */
    void unlock() noexcept {
        state_.store(0, std::memory_order_release);
    }

private:
    // 0: unlocked
    // >0: readers count
    // -1: writer locked
    std::atomic<int32_t> state_{0};
};


/**
 * Backoff 전략
 *
 * 충돌 시 대기 시간 증가로 스래싱 방지
 */
class ExponentialBackoff {
public:
    explicit ExponentialBackoff(uint32_t initial_spins = 4,
                                 uint32_t max_spins = 1024) noexcept
        : current_(initial_spins)
        , initial_(initial_spins)
        , max_(max_spins)
    {}

    /**
     * 백오프 수행
     */
    void backoff() noexcept {
        SpinWait::spin(current_);
        current_ = std::min(current_ * 2, max_);
    }

    /**
     * 리셋
     */
    void reset() noexcept {
        current_ = initial_;
    }

private:
    uint32_t current_;
    uint32_t initial_;
    uint32_t max_;
};

}  // namespace arbitrage

#undef CPU_PAUSE
