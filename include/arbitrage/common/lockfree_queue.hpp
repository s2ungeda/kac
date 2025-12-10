#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <optional>

namespace arbitrage {

// Single Producer Single Consumer Lock-Free Queue
template<typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t capacity) 
        : capacity_(capacity)
        , buffer_(capacity)
        , head_(0)
        , tail_(0) {}
    
    // Producer 스레드에서 호출
    bool push(const T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) % capacity_;
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            // 큐가 가득 참
            return false;
        }
        
        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    bool push(T&& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) % capacity_;
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            // 큐가 가득 참
            return false;
        }
        
        buffer_[current_tail] = std::move(item);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    
    // Consumer 스레드에서 호출
    std::optional<T> pop() {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        
        if (current_head == tail_.load(std::memory_order_acquire)) {
            // 큐가 비어 있음
            return std::nullopt;
        }
        
        T item = std::move(buffer_[current_head]);
        head_.store((current_head + 1) % capacity_, std::memory_order_release);
        return item;
    }
    
    // 큐 크기 (대략적인 값)
    size_t size() const {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        
        if (current_tail >= current_head) {
            return current_tail - current_head;
        } else {
            return capacity_ - current_head + current_tail;
        }
    }
    
    bool empty() const {
        return head_.load(std::memory_order_relaxed) == 
               tail_.load(std::memory_order_relaxed);
    }
    
    size_t capacity() const { return capacity_ - 1; } // 실제 사용 가능한 크기

private:
    const size_t capacity_;
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_;  // Consumer 소유
    alignas(64) std::atomic<size_t> tail_;  // Producer 소유
};

}  // namespace arbitrage