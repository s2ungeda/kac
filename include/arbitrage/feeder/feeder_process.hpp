#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <vector>
#include <functional>
#include <chrono>

#include "arbitrage/common/types.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/ipc/shm_ring_buffer.hpp"
#include "arbitrage/ipc/shm_latest.hpp"
#include "arbitrage/ipc/shm_manager.hpp"
#include "arbitrage/ipc/ipc_types.hpp"

// Forward declarations (Boost/SSL)
namespace boost::asio {
    class io_context;
    namespace ssl { class context; }
}

namespace arbitrage {

// Forward declarations
class WebSocketClientBase;
struct WebSocketEvent;

// =============================================================================
// FeederConfig
// =============================================================================
struct FeederConfig {
    Exchange exchange{Exchange::Upbit};

    // SHM — Ticker
    std::string shm_name;              // 기본값: shm_names::feed_name(exchange)
    size_t shm_capacity{4096};         // 큐 용량 (power of 2)

    // SHM — OrderBook (단일 슬롯, 큐 아님)
    std::string shm_ob_name;           // 기본값: shm_names::ob_name(exchange)

    // WebSocket
    std::string ws_host;               // 거래소별 기본값
    std::string ws_port{"443"};
    std::string ws_target;             // 거래소별 기본값

    // 심볼
    std::vector<std::string> symbols;  // 거래소 네이티브 형식 (e.g., "KRW-XRP")

    // 설정
    std::string config_path{"config/config.yaml"};
    bool verbose{false};
};

// =============================================================================
// FeederStats - 피더 통계 (atomic)
// =============================================================================
struct FeederStats {
    std::atomic<uint64_t> ticks_received{0};
    std::atomic<uint64_t> ticks_pushed{0};
    std::atomic<uint64_t> ticks_dropped{0};       // 큐 full로 드롭된 수
    std::atomic<uint64_t> ob_received{0};          // 호가 수신 수
    std::atomic<uint64_t> ob_pushed{0};            // 호가 SHM push 수
    std::atomic<uint64_t> ob_dropped{0};           // 호가 큐 full 드롭 수
    std::atomic<uint64_t> ws_reconnects{0};
    std::atomic<uint64_t> ws_errors{0};
    std::chrono::steady_clock::time_point started_at;
};

// =============================================================================
// FeederProcess - 거래소별 Feed Handler 기반 클래스
// =============================================================================
//
// 역할: WebSocket 연결 → 시세 수신 → 파싱 → Ticker → SHM Queue write
//
// 사용법:
//   FeederConfig cfg;
//   cfg.exchange = Exchange::Upbit;
//   cfg.symbols = {"KRW-XRP"};
//   FeederProcess feeder(cfg);
//   feeder.run();   // 블로킹 (SIGTERM으로 종료)
//
class FeederProcess {
public:
    explicit FeederProcess(const FeederConfig& config);
    ~FeederProcess();

    // 복사 금지
    FeederProcess(const FeederProcess&) = delete;
    FeederProcess& operator=(const FeederProcess&) = delete;

    // 메인 이벤트 루프 (블로킹 — SIGTERM/SIGINT로 종료)
    int run();

    // 종료 요청 (시그널 핸들러에서 호출)
    void stop();

    // 상태 조회
    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    const FeederStats& stats() const { return stats_; }
    Exchange exchange() const { return config_.exchange; }

    // CLI 파서
    static FeederConfig parse_args(int argc, char* argv[]);

private:
    // 초기화
    void init_shm();
    void init_websocket();
    void setup_signal_handlers();

    // WebSocket 이벤트 핸들러
    void on_event(const WebSocketEvent& evt);

    // 거래소별 WebSocket 생성
    std::shared_ptr<WebSocketClientBase> create_ws_client(
        boost::asio::io_context& ioc,
        boost::asio::ssl::context& ctx);

    // 거래소별 기본 연결 정보
    void apply_exchange_defaults();

    // 거래소별 심볼 구독
    void subscribe_symbols(std::shared_ptr<WebSocketClientBase>& ws);

    FeederConfig config_;
    FeederStats stats_;
    std::atomic<bool> running_{false};

    // SHM — Ticker (덮어쓰기 링버퍼)
    std::unique_ptr<ShmSegment> shm_segment_;
    ShmRingBuffer<Ticker> shm_queue_;

    // SHM — OrderBook (최신 값만 유지)
    std::unique_ptr<ShmSegment> shm_ob_segment_;
    ShmLatestValue<OrderBook> shm_ob_slot_;

    // Logger
    std::shared_ptr<SimpleLogger> logger_;
};

}  // namespace arbitrage
