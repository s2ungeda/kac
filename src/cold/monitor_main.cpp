// TASK_46: Monitor 실행 파일

#include "arbitrage/cold/monitor_process.hpp"
#include "arbitrage/common/logger.hpp"

int main(int argc, char* argv[]) {
    arbitrage::Logger::init("logs");

    auto config = arbitrage::MonitorProcess::parse_args(argc, argv);
    arbitrage::MonitorProcess process(config);
    return process.run();
}
