// TASK_38: MEXC Feeder 실행 파일
// MEXC WebSocket → SHM SPSC Queue (Ticker POD)

#include "arbitrage/feeder/feeder_process.hpp"

int main(int argc, char* argv[]) {
    auto config = arbitrage::FeederProcess::parse_args(argc, argv);
    config.exchange = arbitrage::Exchange::MEXC;

    arbitrage::FeederProcess feeder(config);
    return feeder.run();
}
