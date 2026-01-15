#include "arbitrage/common/config.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/exchange/upbit/websocket.hpp"
#include "arbitrage/exchange/binance/websocket.hpp"
#include "arbitrage/exchange/bithumb/websocket.hpp"
#include "arbitrage/exchange/mexc/websocket.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <sstream>
#include <algorithm>

using namespace arbitrage;

namespace {
    std::atomic<bool> g_running{true};

    void signal_handler(int signum) {
        std::cout << "\n[SIGNAL] Received signal " << signum << ", shutting down...\n";
        g_running = false;
    }
}

// 데이터 파일 경로
const std::string DATA_DIR = "data/";

// 타임스탬프 생성
std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

// ISO 타임스탬프
std::string get_iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

// 데이터 로거 클래스
class DataLogger {
public:
    DataLogger() {
        // 디렉토리 생성
        system(("mkdir -p " + DATA_DIR).c_str());

        // 가격 CSV 헤더
        std::ofstream price_file(DATA_DIR + "prices.csv");
        price_file << "timestamp,exchange,symbol,price,currency\n";
        price_file.close();

        // 프리미엄 CSV 헤더
        std::ofstream premium_file(DATA_DIR + "premium.csv");
        premium_file << "timestamp,buy_exchange,sell_exchange,premium_pct,buy_price_krw,sell_price_krw,fx_rate\n";
        premium_file.close();
    }

    void log_price(const std::string& exchange, const std::string& symbol,
                   double price, const std::string& currency) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::ofstream file(DATA_DIR + "prices.csv", std::ios::app);
        file << get_timestamp() << ","
             << exchange << ","
             << symbol << ","
             << std::fixed << std::setprecision(6) << price << ","
             << currency << "\n";
        file.close();
    }

    void log_premium(const PremiumInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::ofstream file(DATA_DIR + "premium.csv", std::ios::app);
        file << get_timestamp() << ","
             << exchange_name(info.buy_exchange) << ","
             << exchange_name(info.sell_exchange) << ","
             << std::fixed << std::setprecision(4) << info.premium_pct << ","
             << std::setprecision(2) << info.buy_price << ","
             << info.sell_price << ","
             << std::setprecision(2) << info.fx_rate << "\n";
        file.close();
    }

    void log_fxrate(double rate, const std::string& source) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::ofstream file(DATA_DIR + "fxrate.json");
        file << "{\n"
             << "  \"rate\": " << std::fixed << std::setprecision(2) << rate << ",\n"
             << "  \"source\": \"" << source << "\",\n"
             << "  \"timestamp\": \"" << get_iso_timestamp() << "\"\n"
             << "}\n";
        file.close();
    }

    void log_summary(double upbit, double bithumb, double binance, double mexc,
                     double fx_rate, const PremiumMatrix& matrix) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::ofstream file(DATA_DIR + "summary.json");
        file << "{\n"
             << "  \"timestamp\": \"" << get_iso_timestamp() << "\",\n"
             << "  \"fx_rate\": " << std::fixed << std::setprecision(2) << fx_rate << ",\n"
             << "  \"prices\": {\n"
             << "    \"upbit\": { \"price\": " << std::setprecision(2) << upbit << ", \"currency\": \"KRW\" },\n"
             << "    \"bithumb\": { \"price\": " << bithumb << ", \"currency\": \"KRW\" },\n"
             << "    \"binance\": { \"price\": " << std::setprecision(6) << binance << ", \"currency\": \"USDT\" },\n"
             << "    \"mexc\": { \"price\": " << mexc << ", \"currency\": \"USDT\" }\n"
             << "  },\n"
             << "  \"premium_matrix\": {\n";

        const char* exchanges[] = {"upbit", "bithumb", "binance", "mexc"};
        for (int buy = 0; buy < 4; ++buy) {
            file << "    \"" << exchanges[buy] << "\": {";
            for (int sell = 0; sell < 4; ++sell) {
                if (sell > 0) file << ", ";
                double prem = matrix[buy][sell];
                file << "\"" << exchanges[sell] << "\": ";
                if (std::isnan(prem)) {
                    file << "null";
                } else {
                    file << std::setprecision(4) << prem;
                }
            }
            file << "}" << (buy < 3 ? "," : "") << "\n";
        }

        file << "  }\n"
             << "}\n";
        file.close();
    }

private:
    std::mutex mutex_;
};

// 매트릭스 출력
void print_matrix(const PremiumMatrix& matrix) {
    const char* exchanges[] = {"Upbit", "Bithumb", "Binance", "MEXC"};

    std::cout << "\n=== Premium Matrix (%) ===\n\n";
    std::cout << "       Buy ->\n";
    std::cout << "Sell v ";

    for (int i = 0; i < 4; ++i) {
        std::cout << std::setw(9) << exchanges[i];
    }
    std::cout << "\n";

    for (int sell = 0; sell < 4; ++sell) {
        std::cout << std::setw(7) << exchanges[sell];
        for (int buy = 0; buy < 4; ++buy) {
            double premium = matrix[buy][sell];
            std::cout << std::setw(9) << std::fixed << std::setprecision(2);

            if (std::isnan(premium)) {
                std::cout << "N/A";
            } else if (premium > 3.0) {
                std::cout << "*" << premium << "*";
            } else {
                std::cout << premium;
            }
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    // 설정 파일 경로
    std::string config_path = "config/config.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }

    // 로거 초기화
    Logger::init("logs");
    auto logger = Logger::create("main");

    std::cout << "==============================================\n";
    std::cout << "   Kimchi Arbitrage System (C++)\n";
    std::cout << "==============================================\n\n";

    logger->info("Starting Kimchi Arbitrage System");
    logger->info("Config: {}", config_path);

    // 설정 로드
    Config::instance().load(config_path);

    // 시그널 핸들러
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // IO Context와 SSL Context 생성
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tlsv12_client};
    ssl_ctx.set_default_verify_paths();

    // WebSocket 클라이언트 생성
    auto upbit_ws = std::make_shared<UpbitWebSocket>(ioc, ssl_ctx);
    auto binance_ws = std::make_shared<BinanceWebSocket>(ioc, ssl_ctx);
    auto bithumb_ws = std::make_shared<BithumbWebSocket>(ioc, ssl_ctx);
    auto mexc_ws = std::make_shared<MEXCWebSocket>(ioc, ssl_ctx);

    // FXRate 서비스
    FXRateService fx_service;

    // Premium Calculator
    PremiumCalculator calculator;

    // Data Logger
    DataLogger data_logger;

    // 실시간 가격
    std::atomic<double> price_upbit{0};
    std::atomic<double> price_bithumb{0};
    std::atomic<double> price_binance{0};
    std::atomic<double> price_mexc{0};
    std::atomic<double> current_fx_rate{0};

    // 환율 가져오기
    std::cout << "Fetching FX rate...\n";
    auto fx_result = fx_service.fetch();
    if (fx_result) {
        double rate = fx_result.value().rate;
        current_fx_rate = rate;
        calculator.update_fx_rate(rate);
        data_logger.log_fxrate(rate, fx_result.value().source);
        std::cout << "FX Rate: " << std::fixed << std::setprecision(2)
                  << rate << " KRW/USD (" << fx_result.value().source << ")\n";
    } else {
        current_fx_rate = 1450.0;
        calculator.update_fx_rate(1450.0);
        std::cout << "FX Rate: 1450.0 KRW/USD (default)\n";
    }

    // 프리미엄 콜백 설정 (2% 이상 알림)
    calculator.set_threshold(2.0);
    calculator.on_premium_changed([&data_logger, &logger](const PremiumInfo& info) {
        data_logger.log_premium(info);
        logger->warn("ALERT: Premium {}% detected! {} -> {}",
                     info.premium_pct,
                     exchange_name(info.buy_exchange),
                     exchange_name(info.sell_exchange));
        std::cout << "\n*** PREMIUM ALERT: " << std::fixed << std::setprecision(2)
                  << info.premium_pct << "% ("
                  << exchange_name(info.buy_exchange) << " -> "
                  << exchange_name(info.sell_exchange) << ") ***\n";
    });

    // WebSocket 이벤트 핸들러
    upbit_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker() || evt.is_trade()) {
            double price = evt.ticker().price;
            price_upbit = price;
            calculator.update_price(Exchange::Upbit, price);
            data_logger.log_price("upbit", evt.ticker().symbol, price, "KRW");
        }
    });

    binance_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker() || evt.is_trade()) {
            double price = evt.ticker().price;
            price_binance = price;
            calculator.update_price(Exchange::Binance, price);
            data_logger.log_price("binance", evt.ticker().symbol, price, "USDT");
        }
    });

    bithumb_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker() || evt.is_trade()) {
            double price = evt.ticker().price;
            price_bithumb = price;
            calculator.update_price(Exchange::Bithumb, price);
            data_logger.log_price("bithumb", evt.ticker().symbol, price, "KRW");
        }
    });

    mexc_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker() || evt.is_trade()) {
            double price = evt.ticker().price;
            price_mexc = price;
            calculator.update_price(Exchange::MEXC, price);
            data_logger.log_price("mexc", evt.ticker().symbol, price, "USDT");
        }
    });

    // 심볼 구독
    const auto& primary_symbols = Config::instance().primary_symbols();
    if (primary_symbols.empty()) {
        logger->error("No primary symbols configured");
        std::cerr << "Error: No primary symbols configured\n";
        return 1;
    }

    const auto& symbol = primary_symbols[0];
    std::cout << "Symbol: " << symbol.symbol << "\n\n";

    upbit_ws->subscribe_trade({symbol.upbit});
    binance_ws->subscribe_trade({symbol.binance});
    bithumb_ws->subscribe_trade({symbol.bithumb});
    mexc_ws->subscribe_trade({symbol.mexc});

    // WebSocket 연결
    std::cout << "Connecting to exchanges...\n";
    upbit_ws->connect("api.upbit.com", "443", "/websocket/v1");
    // Binance USDS-M Futures: xrpusdt@aggTrade
    std::string binance_symbol_lower = symbol.binance;
    std::transform(binance_symbol_lower.begin(), binance_symbol_lower.end(),
                   binance_symbol_lower.begin(), ::tolower);
    binance_ws->connect("fstream.binance.com", "443", "/stream?streams=" + binance_symbol_lower + "@aggTrade");
    bithumb_ws->connect("ws-api.bithumb.com", "443", "/websocket/v1");
    mexc_ws->connect("contract.mexc.com", "443", "/edge");

    // IO 스레드 시작
    std::thread io_thread([&ioc]() {
        ioc.run();
    });

    logger->info("System started, press Ctrl+C to stop");
    std::cout << "\nSystem running... Press Ctrl+C to stop.\n";
    std::cout << "Data saved to: " << DATA_DIR << "\n\n";

    // 메인 루프
    auto last_display = std::chrono::steady_clock::now();
    auto last_fx_update = std::chrono::steady_clock::now();
    int display_count = 0;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto now = std::chrono::steady_clock::now();

        // 매 초마다 summary 업데이트
        auto matrix = calculator.get_matrix();
        data_logger.log_summary(price_upbit, price_bithumb, price_binance, price_mexc,
                                current_fx_rate, matrix);

        // 10초마다 상태 출력
        auto display_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_display).count();
        if (display_elapsed >= 10) {
            display_count++;
            std::cout << "[" << get_timestamp() << "] "
                      << "Upbit: " << std::fixed << std::setprecision(0) << price_upbit.load()
                      << " | Bithumb: " << price_bithumb.load()
                      << " | Binance: " << std::setprecision(4) << price_binance.load()
                      << " | MEXC: " << price_mexc.load()
                      << " | FX: " << std::setprecision(2) << current_fx_rate.load()
                      << "\n";

            // 30초마다 매트릭스 출력
            if (display_count % 3 == 0) {
                print_matrix(matrix);
            }

            last_display = now;
        }

        // 30초마다 환율 갱신
        auto fx_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_fx_update).count();
        if (fx_elapsed >= 30) {
            auto fx = fx_service.fetch();
            if (fx) {
                current_fx_rate = fx.value().rate;
                calculator.update_fx_rate(fx.value().rate);
                data_logger.log_fxrate(fx.value().rate, fx.value().source);
            }
            last_fx_update = now;
        }
    }

    // 정리
    std::cout << "\nShutting down...\n";
    logger->info("Shutting down...");

    upbit_ws->disconnect();
    binance_ws->disconnect();
    bithumb_ws->disconnect();
    mexc_ws->disconnect();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ioc.stop();
    io_thread.join();

    // 최종 통계 출력
    std::cout << "\n=== Final Statistics ===\n";
    std::cout << "Last prices:\n";
    std::cout << "  Upbit:   " << std::fixed << std::setprecision(0) << price_upbit.load() << " KRW\n";
    std::cout << "  Bithumb: " << price_bithumb.load() << " KRW\n";
    std::cout << "  Binance: " << std::setprecision(4) << price_binance.load() << " USDT\n";
    std::cout << "  MEXC:    " << price_mexc.load() << " USDT\n";
    std::cout << "  FX Rate: " << std::setprecision(2) << current_fx_rate.load() << " KRW/USD\n";
    std::cout << "\nData saved to: " << DATA_DIR << "\n";

    auto best = calculator.get_best_opportunity();
    if (best) {
        std::cout << "Best premium: " << std::setprecision(2) << best->premium_pct << "% ("
                  << exchange_name(best->buy_exchange) << " -> "
                  << exchange_name(best->sell_exchange) << ")\n";
    }

    Logger::shutdown();
    std::cout << "\nGoodbye!\n";

    return 0;
}
