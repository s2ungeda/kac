#include "arbitrage/strategy/premium_calc.hpp"
#include "arbitrage/common/logger.hpp"
#include "arbitrage/common/fxrate.hpp"
#include "arbitrage/common/config.hpp"
#include "arbitrage/exchange/upbit/websocket.hpp"
#include "arbitrage/exchange/binance/websocket.hpp"
#include "arbitrage/exchange/bithumb/websocket.hpp"
#include "arbitrage/exchange/mexc/websocket.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <sstream>

using namespace arbitrage;

// 데이터 파일 경로
const std::string DATA_DIR = "data/";
const std::string PRICE_FILE = DATA_DIR + "prices.csv";
const std::string PREMIUM_FILE = DATA_DIR + "premium.csv";
const std::string FXRATE_FILE = DATA_DIR + "fxrate.json";
const std::string SUMMARY_FILE = DATA_DIR + "summary.json";

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

// 가격 데이터 저장
class DataLogger {
public:
    DataLogger() {
        // 디렉토리 생성
        system(("mkdir -p " + DATA_DIR).c_str());

        // 가격 CSV 헤더
        std::ofstream price_file(PRICE_FILE);
        price_file << "timestamp,exchange,symbol,price,currency\n";
        price_file.close();

        // 프리미엄 CSV 헤더
        std::ofstream premium_file(PREMIUM_FILE);
        premium_file << "timestamp,buy_exchange,sell_exchange,premium_pct,buy_price_krw,sell_price_krw,fx_rate\n";
        premium_file.close();
    }

    void log_price(const std::string& exchange, const std::string& symbol,
                   double price, const std::string& currency) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::ofstream file(PRICE_FILE, std::ios::app);
        file << get_timestamp() << ","
             << exchange << ","
             << symbol << ","
             << std::fixed << std::setprecision(6) << price << ","
             << currency << "\n";
        file.close();
    }

    void log_premium(const PremiumInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::ofstream file(PREMIUM_FILE, std::ios::app);
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

        std::ofstream file(FXRATE_FILE);
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

        std::ofstream file(SUMMARY_FILE);
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

int main(int argc, char* argv[]) {
    // 실행 시간 (초), 기본 60초
    int duration = 60;
    if (argc > 1) {
        duration = std::atoi(argv[1]);
    }

    Logger::init("data_logger");

    std::cout << "=== Arbitrage Data Logger ===\n";
    std::cout << "Duration: " << duration << " seconds\n";
    std::cout << "Data directory: " << DATA_DIR << "\n\n";

    // Config 로드
    Config::instance().load("config/config.yaml");

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
    DataLogger logger;

    // 실시간 가격
    std::atomic<double> price_upbit{0};
    std::atomic<double> price_bithumb{0};
    std::atomic<double> price_binance{0};
    std::atomic<double> price_mexc{0};
    std::atomic<double> current_fx_rate{0};

    // 환율 가져오기
    auto fx_result = fx_service.fetch();
    if (fx_result) {
        double rate = fx_result.value().rate;
        current_fx_rate = rate;
        calculator.update_fx_rate(rate);
        logger.log_fxrate(rate, fx_result.value().source);
        std::cout << "FX Rate: " << rate << " KRW/USD (" << fx_result.value().source << ")\n";
    } else {
        current_fx_rate = 1450.0;
        calculator.update_fx_rate(1450.0);
        std::cout << "FX Rate: 1450.0 KRW/USD (default)\n";
    }

    // 프리미엄 콜백 설정 (1% 이상 기록)
    calculator.set_threshold(1.0);
    calculator.on_premium_changed([&logger](const PremiumInfo& info) {
        logger.log_premium(info);
    });

    // WebSocket 이벤트 핸들러
    upbit_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker()) {
            double price = evt.ticker().price;
            price_upbit = price;
            calculator.update_price(Exchange::Upbit, price);
            logger.log_price("upbit", evt.ticker().symbol, price, "KRW");
        }
    });

    binance_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker()) {
            double price = evt.ticker().price;
            price_binance = price;
            calculator.update_price(Exchange::Binance, price);
            logger.log_price("binance", evt.ticker().symbol, price, "USDT");
        }
    });

    bithumb_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker()) {
            double price = evt.ticker().price;
            price_bithumb = price;
            calculator.update_price(Exchange::Bithumb, price);
            logger.log_price("bithumb", evt.ticker().symbol, price, "KRW");
        }
    });

    mexc_ws->on_event([&](const WebSocketEvent& evt) {
        if (evt.is_ticker()) {
            double price = evt.ticker().price;
            price_mexc = price;
            calculator.update_price(Exchange::MEXC, price);
            logger.log_price("mexc", evt.ticker().symbol, price, "USDT");
        }
    });

    // 심볼 구독
    const auto& primary_symbols = Config::instance().primary_symbols();
    if (primary_symbols.empty()) {
        std::cerr << "No primary symbols configured\n";
        return 1;
    }

    const auto& symbol = primary_symbols[0];
    std::cout << "Symbol: " << symbol.symbol << "\n\n";

    upbit_ws->subscribe_ticker({symbol.upbit});
    binance_ws->subscribe_ticker({symbol.binance});
    bithumb_ws->subscribe_ticker({symbol.bithumb});
    mexc_ws->subscribe_ticker({symbol.mexc});

    // WebSocket 연결
    std::cout << "Connecting to exchanges...\n";
    upbit_ws->connect("api.upbit.com", "443", "/websocket/v1");
    binance_ws->connect("stream.binance.com", "9443", "/stream?streams=xrpusdt@ticker");
    bithumb_ws->connect("pubwss.bithumb.com", "443", "/pub/ws");
    mexc_ws->connect("wbs-api.mexc.com", "443", "/ws");

    // IO 스레드 시작
    std::thread io_thread([&ioc]() {
        ioc.run();
    });

    // 데이터 수집 루프
    std::cout << "Logging data for " << duration << " seconds...\n\n";

    auto start_time = std::chrono::steady_clock::now();
    int log_count = 0;

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();

        if (elapsed >= duration) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 매 초마다 summary 업데이트
        auto matrix = calculator.get_matrix();
        logger.log_summary(price_upbit, price_bithumb, price_binance, price_mexc,
                          current_fx_rate, matrix);

        // 10초마다 상태 출력
        if (elapsed % 10 == 0 && elapsed > 0) {
            std::cout << "[" << elapsed << "s] "
                      << "Upbit: " << std::fixed << std::setprecision(0) << price_upbit.load()
                      << " | Bithumb: " << price_bithumb.load()
                      << " | Binance: " << std::setprecision(4) << price_binance.load()
                      << " | MEXC: " << price_mexc.load()
                      << " | FX: " << std::setprecision(2) << current_fx_rate.load()
                      << "\n";
            log_count++;
        }

        // 환율 갱신 (30초마다)
        if (elapsed % 30 == 0 && elapsed > 0) {
            auto fx = fx_service.fetch();
            if (fx) {
                current_fx_rate = fx.value().rate;
                calculator.update_fx_rate(fx.value().rate);
                logger.log_fxrate(fx.value().rate, fx.value().source);
            }
        }
    }

    // 정리
    std::cout << "\nShutting down...\n";

    upbit_ws->disconnect();
    binance_ws->disconnect();
    bithumb_ws->disconnect();
    mexc_ws->disconnect();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ioc.stop();
    io_thread.join();

    // 결과 출력
    std::cout << "\n=== Data Logging Complete ===\n";
    std::cout << "Files saved to: " << DATA_DIR << "\n";
    std::cout << "  - prices.csv    : Price history\n";
    std::cout << "  - premium.csv   : Premium alerts (>1%)\n";
    std::cout << "  - fxrate.json   : Current FX rate\n";
    std::cout << "  - summary.json  : Latest summary\n";

    return 0;
}
