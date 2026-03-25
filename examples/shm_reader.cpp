/**
 * SHM Reader — Ticker + OrderBook SHM 큐 진단 도구
 *
 * 사용법:
 *   ./shm_reader                    # 4개 거래소 전부 읽기 (기본 5초)
 *   ./shm_reader --exchange upbit   # 업비트만
 *   ./shm_reader --duration 10      # 10초간 수집
 *   ./shm_reader --ob-only          # 호가만 출력
 *
 * 선행조건: 해당 거래소 feeder가 실행 중이어야 SHM 세그먼트 존재
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <string>
#include <array>

#include "arbitrage/common/types.hpp"
#include "arbitrage/ipc/ipc_types.hpp"
#include "arbitrage/ipc/shm_ring_buffer.hpp"
#include "arbitrage/ipc/shm_latest.hpp"
#include "arbitrage/ipc/shm_manager.hpp"

using namespace arbitrage;

struct ReaderConfig {
    int exchange_filter = -1;  // -1 = all
    int duration_sec = 5;
    bool ob_only = false;
    bool tick_only = false;
};

struct FeedReader {
    Exchange exchange;
    std::unique_ptr<ShmSegment> tick_seg;
    ShmRingBuffer<Ticker> tick_queue;
    std::unique_ptr<ShmSegment> ob_seg;
    ShmLatestValue<OrderBook> ob_slot;
    bool tick_ok = false;
    bool ob_ok = false;
    uint64_t tick_count = 0;
    uint64_t ob_count = 0;
    uint64_t last_ob_seq = 0;  // 변경 감지용
};

static ReaderConfig parse_args(int argc, char* argv[]) {
    ReaderConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--exchange" || arg == "-e") && i + 1 < argc) {
            std::string ex = argv[++i];
            if (ex == "upbit")        cfg.exchange_filter = 0;
            else if (ex == "bithumb") cfg.exchange_filter = 1;
            else if (ex == "binance") cfg.exchange_filter = 2;
            else if (ex == "mexc")    cfg.exchange_filter = 3;
        } else if ((arg == "--duration" || arg == "-d") && i + 1 < argc) {
            cfg.duration_sec = std::atoi(argv[++i]);
        } else if (arg == "--ob-only") {
            cfg.ob_only = true;
        } else if (arg == "--tick-only") {
            cfg.tick_only = true;
        } else if (arg == "--help" || arg == "-h") {
            printf("Usage: %s [options]\n"
                   "  --exchange, -e  upbit|bithumb|binance|mexc (default: all)\n"
                   "  --duration, -d  seconds to read (default: 5)\n"
                   "  --ob-only       orderbook only\n"
                   "  --tick-only     ticker only\n", argv[0]);
            std::exit(0);
        }
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    constexpr size_t TICK_CAPACITY = 4096;
    constexpr size_t TICK_SHM_SIZE = shm_queue_size(TICK_CAPACITY, sizeof(Ticker));
    const size_t OB_SHM_SIZE = shm_latest_size<OrderBook>();

    const Exchange exchanges[] = {
        Exchange::Upbit, Exchange::Bithumb, Exchange::Binance, Exchange::MEXC
    };

    std::array<FeedReader, 4> readers;

    printf("=== SHM Reader — Ticker + OrderBook ===\n\n");

    // Attach to SHM segments
    for (int i = 0; i < 4; ++i) {
        if (cfg.exchange_filter >= 0 && cfg.exchange_filter != i) continue;

        readers[i].exchange = exchanges[i];
        const char* tick_name = shm_names::feed_name(exchanges[i]);
        const char* ob_name = shm_names::ob_name(exchanges[i]);

        // Ticker SHM (RingBuffer)
        if (!cfg.ob_only) {
            try {
                readers[i].tick_seg = std::make_unique<ShmSegment>(tick_name, TICK_SHM_SIZE, false);
                readers[i].tick_queue = ShmRingBuffer<Ticker>::attach_consumer(
                    readers[i].tick_seg->data());
                readers[i].tick_ok = readers[i].tick_queue.valid();
                if (readers[i].tick_ok) {
                    printf("[%s] Ticker RingBuffer attached: %s (producer pid=%d)\n",
                        exchange_name(exchanges[i]), tick_name,
                        readers[i].tick_queue.producer_pid());
                }
            } catch (...) {
                printf("[%s] Ticker SHM not found: %s\n",
                    exchange_name(exchanges[i]), tick_name);
            }
        }

        // OrderBook SHM (LatestValue)
        if (!cfg.tick_only) {
            try {
                readers[i].ob_seg = std::make_unique<ShmSegment>(ob_name, OB_SHM_SIZE, false);
                readers[i].ob_slot = ShmLatestValue<OrderBook>::attach_consumer(
                    readers[i].ob_seg->data());
                readers[i].ob_ok = readers[i].ob_slot.valid();
                if (readers[i].ob_ok) {
                    printf("[%s] OrderBook SHM slot attached: %s (producer pid=%d)\n",
                        exchange_name(exchanges[i]), ob_name,
                        readers[i].ob_slot.producer_pid());
                }
            } catch (...) {
                printf("[%s] OrderBook SHM not found: %s\n",
                    exchange_name(exchanges[i]), ob_name);
            }
        }
    }

    printf("\nReading for %d seconds...\n\n", cfg.duration_sec);

    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::seconds(cfg.duration_sec);

    Ticker ticker;
    OrderBook ob;

    while (std::chrono::steady_clock::now() < deadline) {
        bool had_work = false;

        for (int i = 0; i < 4; ++i) {
            if (cfg.exchange_filter >= 0 && cfg.exchange_filter != i) continue;

            // Ticker
            while (readers[i].tick_ok && readers[i].tick_queue.pop(ticker)) {
                had_work = true;
                readers[i].tick_count++;
                printf("[TICK] %-8s %s  price=%.4f  bid=%.4f  ask=%.4f  vol=%.2f\n",
                    exchange_name(ticker.exchange),
                    ticker.symbol,
                    ticker.price, ticker.bid, ticker.ask,
                    ticker.volume_24h);
            }

            // OrderBook (최신 값 — 변경 시에만 출력)
            if (readers[i].ob_ok) {
                uint64_t cur_seq = readers[i].ob_slot.sequence();
                if (cur_seq != readers[i].last_ob_seq && readers[i].ob_slot.load(ob)) {
                    readers[i].last_ob_seq = readers[i].ob_slot.sequence();
                    had_work = true;
                    readers[i].ob_count++;
                    printf("[OB]   %-8s %s  bids=%d asks=%d  best_bid=%.4f best_ask=%.4f  spread=%.4f\n",
                        exchange_name(ob.exchange),
                        ob.symbol,
                        ob.bid_count, ob.ask_count,
                        ob.best_bid(), ob.best_ask(),
                        ob.spread());

                    // Top 3 호가 출력
                    int show = (ob.bid_count < 3) ? ob.bid_count : 3;
                    for (int j = 0; j < show; ++j) {
                        printf("       bid[%d] %.4f x %.4f   ask[%d] %.4f x %.4f\n",
                            j, ob.bids[j].price, ob.bids[j].quantity,
                            j, ob.asks[j].price, ob.asks[j].quantity);
                    }
                }
            }
        }

        if (!had_work) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    // Summary
    printf("\n=== Summary (%d seconds) ===\n", cfg.duration_sec);
    for (int i = 0; i < 4; ++i) {
        if (cfg.exchange_filter >= 0 && cfg.exchange_filter != i) continue;
        printf("  %-8s  ticks=%lu  orderbooks=%lu\n",
            exchange_name(exchanges[i]),
            readers[i].tick_count,
            readers[i].ob_count);
    }

    return 0;
}
