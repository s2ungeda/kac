#pragma once

#include <string>
#include <cstddef>

namespace arbitrage {

// =============================================================================
// ShmSegment - POSIX Shared Memory RAII Wrapper
// =============================================================================
//
// 생성자(create=true): shm_open(O_CREAT) + ftruncate + mmap
// 생성자(create=false): shm_open(O_RDWR) + mmap
// 소멸자: munmap, 생성자면 shm_unlink
//
class ShmSegment {
public:
    // 생성 또는 연결
    // @param name  SHM 이름 (예: "/kimchi_feed_upbit")
    // @param size  세그먼트 크기 (bytes)
    // @param create  true: 새로 생성, false: 기존 세그먼트에 연결
    ShmSegment(const std::string& name, size_t size, bool create);

    ~ShmSegment();

    // 복사 금지
    ShmSegment(const ShmSegment&) = delete;
    ShmSegment& operator=(const ShmSegment&) = delete;

    // 이동 허용
    ShmSegment(ShmSegment&& other) noexcept;
    ShmSegment& operator=(ShmSegment&& other) noexcept;

    // 매핑된 메모리 포인터
    void* data() { return data_; }
    const void* data() const { return data_; }

    // 세그먼트 크기
    size_t size() const { return size_; }

    // SHM 이름
    const std::string& name() const { return name_; }

    // 유효성 검사
    bool valid() const { return data_ != nullptr; }

    // 수동으로 SHM 세그먼트 삭제 (unlink)
    // 이미 소멸자에서 호출되지만, 명시적으로도 호출 가능
    static void unlink(const std::string& name);

private:
    std::string name_;
    size_t size_{0};
    void* data_{nullptr};
    int fd_{-1};
    bool owner_{false};  // 생성자(creator)인 경우 true → 소멸 시 unlink

    void cleanup();
};

}  // namespace arbitrage
