#include "arbitrage/ipc/shm_manager.hpp"
#include "arbitrage/common/logger.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace arbitrage {

ShmSegment::ShmSegment(const std::string& name, size_t size, bool create)
    : name_(name)
    , size_(size)
    , owner_(create)
{
    int flags = O_RDWR;
    if (create) {
        flags |= O_CREAT | O_EXCL;
    }

    fd_ = ::shm_open(name.c_str(), flags, 0666);
    if (fd_ < 0) {
        if (create && errno == EEXIST) {
            // 이전 세그먼트가 남아있으면 삭제 후 재생성
            ::shm_unlink(name.c_str());
            fd_ = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        }
        if (fd_ < 0) {
            Logger::get("shm")->error("shm_open('{}') failed: {} ({})",
                                       name, std::strerror(errno), errno);
            throw std::runtime_error("shm_open failed: " + name
                                     + " - " + std::strerror(errno));
        }
    }

    if (create) {
        if (::ftruncate(fd_, static_cast<off_t>(size)) < 0) {
            Logger::get("shm")->error("ftruncate('{}', {}) failed: {}",
                                       name, size, std::strerror(errno));
            ::close(fd_);
            ::shm_unlink(name.c_str());
            fd_ = -1;
            throw std::runtime_error("ftruncate failed: " + name
                                     + " - " + std::strerror(errno));
        }
    }

    data_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        Logger::get("shm")->error("mmap('{}', {}) failed: {}",
                                   name, size, std::strerror(errno));
        ::close(fd_);
        if (create) ::shm_unlink(name.c_str());
        fd_ = -1;
        data_ = nullptr;
        throw std::runtime_error("mmap failed: " + name
                                 + " - " + std::strerror(errno));
    }

    // 생성자에서 zero 초기화
    if (create) {
        std::memset(data_, 0, size);
    }

    // fd는 mmap 이후 닫아도 됨 (매핑 유지)
    ::close(fd_);
    fd_ = -1;

    Logger::get("shm")->info("ShmSegment {} {} bytes ({})",
                              create ? "created" : "attached",
                              size, name);
}

ShmSegment::~ShmSegment() {
    cleanup();
}

ShmSegment::ShmSegment(ShmSegment&& other) noexcept
    : name_(std::move(other.name_))
    , size_(other.size_)
    , data_(other.data_)
    , fd_(other.fd_)
    , owner_(other.owner_)
{
    other.data_ = nullptr;
    other.fd_ = -1;
    other.size_ = 0;
    other.owner_ = false;
}

ShmSegment& ShmSegment::operator=(ShmSegment&& other) noexcept {
    if (this != &other) {
        cleanup();
        name_ = std::move(other.name_);
        size_ = other.size_;
        data_ = other.data_;
        fd_ = other.fd_;
        owner_ = other.owner_;
        other.data_ = nullptr;
        other.fd_ = -1;
        other.size_ = 0;
        other.owner_ = false;
    }
    return *this;
}

void ShmSegment::unlink(const std::string& name) {
    if (::shm_unlink(name.c_str()) < 0 && errno != ENOENT) {
        Logger::get("shm")->warn("shm_unlink('{}') failed: {}",
                                  name, std::strerror(errno));
    }
}

void ShmSegment::cleanup() {
    if (data_) {
        ::munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (owner_ && !name_.empty()) {
        ::shm_unlink(name_.c_str());
        Logger::get("shm")->info("ShmSegment unlinked: {}", name_);
    }
}

}  // namespace arbitrage
