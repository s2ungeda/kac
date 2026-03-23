// TASK_38: Upbit Feeder 실행 파일
// 업비트 WebSocket → SHM SPSC Queue (Ticker POD)

#include "arbitrage/feeder/feeder_process.hpp"

int main(int argc, char* argv[]) {
    auto config = arbitrage::FeederProcess::parse_args(argc, argv);
    config.exchange = arbitrage::Exchange::Upbit;

    arbitrage::FeederProcess feeder(config);
    return feeder.run();
}
