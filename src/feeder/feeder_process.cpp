#include "arbitrage/feeder/feeder_process.hpp"
#include "arbitrage/exchange/websocket_base.hpp"
#include "arbitrage/exchange/upbit/websocket.hpp"
#include "arbitrage/exchange/binance/websocket.hpp"
#include "arbitrage/exchange/bithumb/websocket.hpp"
#include "arbitrage/exchange/mexc/websocket.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <csignal>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <thread>

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;

namespace arbitrage {

// =============================================================================
// 글로벌 시그널 핸들러용 포인터
// =============================================================================
static std::atomic<FeederProcess*> g_feeder_instance{nullptr};

static void feeder_signal_handler(int sig) {
    auto* feeder = g_feeder_instance.load(std::memory_order_relaxed);
    if (feeder) {
        feeder->stop();
    }
}

// =============================================================================
// 생성자/소멸자
// =============================================================================
FeederProcess::FeederProcess(const FeederConfig& config)
    : config_(config)
    , logger_(Logger::get(std::string("feeder.") + exchange_name(config.exchange)))
{
    apply_exchange_defaults();
}

FeederProcess::~FeederProcess() {
    if (running_.load()) {
        stop();
    }
    // 글로벌 포인터 해제
    g_feeder_instance.compare_exchange_strong(
        const_cast<FeederProcess*&>(
            reinterpret_cast<FeederProcess*&>(g_feeder_instance)),
        nullptr);
}

// =============================================================================
// 거래소별 기본값 적용
// =============================================================================
void FeederProcess::apply_exchange_defaults() {
    // SHM 이름 기본값 (Ticker)
    if (config_.shm_name.empty()) {
        const char* name = shm_names::feed_name(config_.exchange);
        if (name) config_.shm_name = name;
        else config_.shm_name = "/kimchi_feed_unknown";
    }

    // SHM 이름 기본값 (OrderBook)
    if (config_.shm_ob_name.empty()) {
        const char* name = shm_names::ob_name(config_.exchange);
        if (name) config_.shm_ob_name = name;
        else config_.shm_ob_name = "/kimchi_ob_unknown";
    }

    // WebSocket 연결 정보 기본값
    if (config_.ws_host.empty()) {
        switch (config_.exchange) {
            case Exchange::Upbit:
                config_.ws_host = "api.upbit.com";
                config_.ws_target = "/websocket/v1";
                break;
            case Exchange::Bithumb:
                config_.ws_host = "ws-api.bithumb.com";
                config_.ws_target = "/websocket/v1";
                break;
            case Exchange::Binance:
                config_.ws_host = "fstream.binance.com";
                // target은 심볼에 따라 동적 설정 (subscribe_symbols에서)
                break;
            case Exchange::MEXC:
                config_.ws_host = "contract.mexc.com";
                config_.ws_target = "/edge";
                break;
            default:
                break;
        }
    }

    // 기본 심볼
    if (config_.symbols.empty()) {
        switch (config_.exchange) {
            case Exchange::Upbit:   config_.symbols = {"KRW-XRP"}; break;
            case Exchange::Bithumb: config_.symbols = {"KRW-XRP"}; break;
            case Exchange::Binance: config_.symbols = {"XRPUSDT"}; break;
            case Exchange::MEXC:    config_.symbols = {"XRPUSDT"}; break;
            default: break;
        }
    }
}

// =============================================================================
// SHM 초기화
// =============================================================================
void FeederProcess::init_shm() {
    // Ticker SHM (덮어쓰기 링버퍼 — 오래된 데이터 자동 폐기)
    size_t shm_size = shm_queue_size(config_.shm_capacity, sizeof(Ticker));

    logger_->info("Creating Ticker RingBuffer SHM: {} ({} bytes, capacity={})",
                  config_.shm_name, shm_size, config_.shm_capacity);

    shm_segment_ = std::make_unique<ShmSegment>(
        config_.shm_name, shm_size, true);

    shm_queue_ = ShmRingBuffer<Ticker>::init_producer(
        shm_segment_->data(), config_.shm_capacity);

    if (!shm_queue_.valid()) {
        throw std::runtime_error("Failed to init Ticker RingBuffer: " + config_.shm_name);
    }

    logger_->info("Ticker RingBuffer ready: {} (pid={})",
                  config_.shm_name, shm_queue_.producer_pid());

    // OrderBook SHM (최신 값만 유지 — seqlock 기반)
    size_t ob_shm_size = shm_latest_size<OrderBook>();

    logger_->info("Creating OrderBook SHM slot: {} ({} bytes)",
                  config_.shm_ob_name, ob_shm_size);

    shm_ob_segment_ = std::make_unique<ShmSegment>(
        config_.shm_ob_name, ob_shm_size, true);

    shm_ob_slot_ = ShmLatestValue<OrderBook>::init_producer(
        shm_ob_segment_->data(), 0);

    if (!shm_ob_slot_.valid()) {
        throw std::runtime_error("Failed to init OrderBook SHM slot: " + config_.shm_ob_name);
    }

    logger_->info("OrderBook SHM slot ready: {} (pid={})",
                  config_.shm_ob_name, shm_ob_slot_.producer_pid());
}

// =============================================================================
// WebSocket 클라이언트 생성
// =============================================================================
std::shared_ptr<WebSocketClientBase> FeederProcess::create_ws_client(
    net::io_context& ioc, ssl::context& ctx)
{
    switch (config_.exchange) {
        case Exchange::Upbit:
            return std::make_shared<UpbitWebSocket>(ioc, ctx);
        case Exchange::Bithumb:
            return std::make_shared<BithumbWebSocket>(ioc, ctx);
        case Exchange::Binance:
            return std::make_shared<BinanceWebSocket>(ioc, ctx);
        case Exchange::MEXC:
            return std::make_shared<MEXCWebSocket>(ioc, ctx);
        default:
            throw std::runtime_error("Unknown exchange: "
                + std::to_string(static_cast<int>(config_.exchange)));
    }
}

// =============================================================================
// 심볼 구독
// =============================================================================
void FeederProcess::subscribe_symbols(std::shared_ptr<WebSocketClientBase>& ws) {
    switch (config_.exchange) {
        case Exchange::Upbit: {
            auto upbit = std::dynamic_pointer_cast<UpbitWebSocket>(ws);
            if (upbit) {
                upbit->subscribe_trade(config_.symbols);
                upbit->subscribe_orderbook(config_.symbols);
            }
            break;
        }
        case Exchange::Bithumb: {
            auto bithumb = std::dynamic_pointer_cast<BithumbWebSocket>(ws);
            if (bithumb) {
                bithumb->subscribe_trade(config_.symbols);
                bithumb->subscribe_orderbook(config_.symbols);
            }
            break;
        }
        case Exchange::Binance: {
            auto binance = std::dynamic_pointer_cast<BinanceWebSocket>(ws);
            if (binance) {
                binance->subscribe_trade(config_.symbols);
                binance->subscribe_orderbook(config_.symbols, 10);
            }

            // Binance: target에 stream 파라미터 포함 (trade + depth)
            if (config_.ws_target.empty() && !config_.symbols.empty()) {
                std::string sym = config_.symbols[0];
                std::transform(sym.begin(), sym.end(), sym.begin(), ::tolower);
                config_.ws_target = "/stream?streams=" + sym + "@aggTrade/"
                                  + sym + "@depth10";
            }
            break;
        }
        case Exchange::MEXC: {
            auto mexc = std::dynamic_pointer_cast<MEXCWebSocket>(ws);
            if (mexc) {
                mexc->subscribe_trade(config_.symbols);
                mexc->subscribe_orderbook(config_.symbols);
            }
            break;
        }
        default:
            break;
    }
}

// =============================================================================
// WebSocket 이벤트 핸들러
// =============================================================================
void FeederProcess::on_event(const WebSocketEvent& evt) {
    if (evt.is_ticker() || evt.is_trade()) {
        stats_.ticks_received.fetch_add(1, std::memory_order_relaxed);

        const Ticker& ticker = evt.ticker();

        // 링버퍼: 항상 성공, 오래된 데이터 자동 폐기
        shm_queue_.push(ticker);
        stats_.ticks_pushed.fetch_add(1, std::memory_order_relaxed);
    } else if (evt.is_orderbook()) {
        stats_.ob_received.fetch_add(1, std::memory_order_relaxed);

        const OrderBook& ob = evt.orderbook();

        // 최신 값 덮어쓰기 (seqlock)
        shm_ob_slot_.store(ob);
        stats_.ob_pushed.fetch_add(1, std::memory_order_relaxed);
    } else if (evt.type == WebSocketEvent::Type::Connected) {
        logger_->info("WebSocket connected to {}",
                      exchange_name(config_.exchange));
    } else if (evt.type == WebSocketEvent::Type::Disconnected) {
        stats_.ws_reconnects.fetch_add(1, std::memory_order_relaxed);
        logger_->warn("WebSocket disconnected from {} (reconnecting...)",
                      exchange_name(config_.exchange));
    } else if (evt.type == WebSocketEvent::Type::Error) {
        stats_.ws_errors.fetch_add(1, std::memory_order_relaxed);
        logger_->error("WebSocket error from {}: {}",
                       exchange_name(config_.exchange),
                       evt.error_message);
    }
}

// =============================================================================
// 시그널 핸들러 설정
// =============================================================================
void FeederProcess::setup_signal_handlers() {
    g_feeder_instance.store(this, std::memory_order_relaxed);

    struct sigaction sa{};
    sa.sa_handler = feeder_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// =============================================================================
// stop()
// =============================================================================
void FeederProcess::stop() {
    running_.store(false, std::memory_order_relaxed);
    logger_->info("Stop requested for {} feeder",
                  exchange_name(config_.exchange));
}

// =============================================================================
// run() - 메인 이벤트 루프
// =============================================================================
int FeederProcess::run() {
    logger_->info("Starting {} feeder (pid={}, symbols={})",
                  exchange_name(config_.exchange),
                  ::getpid(),
                  config_.symbols.empty() ? "default" : config_.symbols[0]);

    // 1. 시그널 핸들러
    setup_signal_handlers();

    // 2. SHM 초기화
    try {
        init_shm();
    } catch (const std::exception& e) {
        logger_->error("SHM init failed: {}", e.what());
        return 1;
    }

    // 3. Boost.Asio + SSL
    net::io_context ioc;
    ssl::context ssl_ctx(ssl::context::tlsv12_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(ssl::verify_none);

    // 4. WebSocket 생성 + 구독 + 이벤트 콜백
    auto ws_client = create_ws_client(ioc, ssl_ctx);

    subscribe_symbols(ws_client);

    ws_client->on_event([this](const WebSocketEvent& evt) {
        on_event(evt);
    });

    // 5. 연결
    logger_->info("Connecting to {}:{}{}", config_.ws_host, config_.ws_port, config_.ws_target);
    ws_client->connect(config_.ws_host, config_.ws_port, config_.ws_target);

    // 6. 이벤트 루프 시작
    running_.store(true, std::memory_order_relaxed);
    stats_.started_at = std::chrono::steady_clock::now();

    // IO 스레드 (Boost.Asio 이벤트 루프)
    std::thread io_thread([&ioc, this]() {
        try {
            ioc.run();
        } catch (const std::exception& e) {
            logger_->error("IO thread exception: {}", e.what());
        }
        // io_context 종료 시 running도 false
        running_.store(false, std::memory_order_relaxed);
    });

    // 메인 스레드: running 플래그 감시 + 통계 로깅
    while (running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(10));

        if (running_.load(std::memory_order_relaxed)) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - stats_.started_at).count();

            logger_->info("{} feeder: ticks={}/{} ob={}/{} dropped={}/{} reconnects={} ({}s)",
                exchange_name(config_.exchange),
                stats_.ticks_received.load(std::memory_order_relaxed),
                stats_.ticks_pushed.load(std::memory_order_relaxed),
                stats_.ob_received.load(std::memory_order_relaxed),
                stats_.ob_pushed.load(std::memory_order_relaxed),
                stats_.ticks_dropped.load(std::memory_order_relaxed),
                stats_.ob_dropped.load(std::memory_order_relaxed),
                stats_.ws_reconnects.load(std::memory_order_relaxed),
                elapsed);
        }
    }

    // 7. 종료
    logger_->info("Shutting down {} feeder...", exchange_name(config_.exchange));

    ws_client->disconnect();
    ioc.stop();

    if (io_thread.joinable()) {
        io_thread.join();
    }

    // SHM close
    shm_queue_.close();
    shm_ob_slot_.close();

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - stats_.started_at).count();

    logger_->info("{} feeder stopped: ticks={}/{} ob={}/{} ({}s)",
        exchange_name(config_.exchange),
        stats_.ticks_received.load(),
        stats_.ticks_pushed.load(),
        stats_.ob_received.load(),
        stats_.ob_pushed.load(),
        elapsed);

    return 0;
}

// =============================================================================
// CLI 파서
// =============================================================================
FeederConfig FeederProcess::parse_args(int argc, char* argv[]) {
    FeederConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if ((arg == "--exchange" || arg == "-e") && i + 1 < argc) {
            std::string ex_str = argv[++i];
            if (ex_str == "upbit")        cfg.exchange = Exchange::Upbit;
            else if (ex_str == "bithumb") cfg.exchange = Exchange::Bithumb;
            else if (ex_str == "binance") cfg.exchange = Exchange::Binance;
            else if (ex_str == "mexc")    cfg.exchange = Exchange::MEXC;
            else {
                std::cerr << "Unknown exchange: " << ex_str << "\n";
                std::exit(1);
            }
        }
        else if ((arg == "--shm" || arg == "-s") && i + 1 < argc) {
            cfg.shm_name = argv[++i];
        }
        else if ((arg == "--capacity" || arg == "-c") && i + 1 < argc) {
            cfg.shm_capacity = std::stoull(argv[++i]);
        }
        else if ((arg == "--symbol") && i + 1 < argc) {
            cfg.symbols.push_back(argv[++i]);
        }
        else if ((arg == "--config") && i + 1 < argc) {
            cfg.config_path = argv[++i];
        }
        else if ((arg == "--host") && i + 1 < argc) {
            cfg.ws_host = argv[++i];
        }
        else if ((arg == "--port") && i + 1 < argc) {
            cfg.ws_port = argv[++i];
        }
        else if ((arg == "--target") && i + 1 < argc) {
            cfg.ws_target = argv[++i];
        }
        else if (arg == "--config-stdin") {
            cfg.config_from_stdin = true;
        }
        else if (arg == "--verbose" || arg == "-v") {
            cfg.verbose = true;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --exchange, -e  Exchange name (upbit|bithumb|binance|mexc)\n"
                      << "  --shm, -s       SHM segment name (default: auto)\n"
                      << "  --capacity, -c  SHM queue capacity (default: 4096)\n"
                      << "  --symbol        Trading symbol (can repeat)\n"
                      << "  --config        Config file path\n"
                      << "  --host          WebSocket host override\n"
                      << "  --port          WebSocket port override\n"
                      << "  --target        WebSocket target override\n"
                      << "  --verbose, -v   Verbose logging\n"
                      << "  --help, -h      Show this help\n";
            std::exit(0);
        }
    }

    return cfg;
}

}  // namespace arbitrage
