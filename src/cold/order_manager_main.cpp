// TASK_44: Order Manager 실행 파일
// SHM Queue → DualOrderExecutor → SHM Result + UDS Risk

#include "arbitrage/cold/order_manager_process.hpp"
#include "arbitrage/common/logger.hpp"

int main(int argc, char* argv[]) {
    arbitrage::Logger::init("logs");

    auto config = arbitrage::OrderManagerProcess::parse_args(argc, argv);
    arbitrage::OrderManagerProcess process(config);
    return process.run();
}
