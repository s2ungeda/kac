# TASK_36: Shared Memory SPSC Queue

> Phase 2/3 공통 기반

---

## 📌 목표

프로세스 간 Lock-Free 통신을 위한 Shared Memory SPSC Queue 구현

---

## 🔧 설계

### SHM 메모리 레이아웃

```
Offset 0:    ShmQueueHeader  (64 bytes, cache-line)
  - uint64_t magic           0xDEADBEEF (유효성 검사)
  - uint64_t version         프로토콜 버전
  - uint64_t capacity        링 버퍼 크기 (power of 2)
  - uint64_t element_size    sizeof(T)
  - pid_t    producer_pid    Writer PID
  - pid_t    consumer_pid    Reader PID
  - uint8_t  state           0=init, 1=ready, 2=closed
  - padding

Offset 64:   atomic<size_t> head   (64 bytes, cache-line padding)
Offset 128:  atomic<size_t> tail   (64 bytes, cache-line padding)
Offset 192:  T buffer[capacity]    (capacity * sizeof(T))
```

Ticker(64B) × 4096 = 약 256KB per queue

### ShmSPSCQueue<T> API

```cpp
// Producer 측
static ShmSPSCQueue<T> init_producer(void* mem, size_t capacity);
bool push(const T& item);
void close();  // state = closed

// Consumer 측
static ShmSPSCQueue<T> attach_consumer(void* mem);
bool pop(T& item);
bool is_closed() const;
bool is_producer_alive() const;  // kill(pid, 0)
```

### ShmSegment RAII Wrapper

```cpp
class ShmSegment {
    ShmSegment(const std::string& name, size_t size, bool create);
    ~ShmSegment();  // munmap, 생성자면 shm_unlink
    void* data();
    size_t size();
};
```

### SHM 이름 규칙

| 세그먼트 | 이름 | 용도 |
|----------|------|------|
| Upbit Feed | `/kimchi_feed_upbit` | Feeder → Engine |
| Bithumb Feed | `/kimchi_feed_bithumb` | Feeder → Engine |
| Binance Feed | `/kimchi_feed_binance` | Feeder → Engine |
| MEXC Feed | `/kimchi_feed_mexc` | Feeder → Engine |
| Order Request | `/kimchi_orders` | Engine → OrderManager |
| Order Result | `/kimchi_order_results` | OrderManager → Engine |

---

## 📁 생성 파일

| 파일 | 내용 |
|------|------|
| `include/arbitrage/ipc/shm_queue.hpp` | ShmSPSCQueue<T> |
| `include/arbitrage/ipc/shm_manager.hpp` | ShmSegment RAII |
| `include/arbitrage/ipc/ipc_types.hpp` | ShmQueueHeader, IpcMessage |
| `src/ipc/shm_manager.cpp` | 구현 |
| `src/ipc/CMakeLists.txt` | 빌드 설정 |

---

## ✅ 완료 조건

- [ ] ShmSPSCQueue<Ticker> push/pop 동작
- [ ] static_assert(std::atomic<size_t>::is_always_lock_free)
- [ ] static_assert(std::is_trivially_copyable_v<Ticker>)
- [ ] fork() 테스트: 부모 write → 자식 read 검증
- [ ] ShmSegment 생성/해제 (shm_unlink)
- [ ] 빌드 성공

## 📎 의존성: 없음
## ⏱️ 예상: 1.5일
