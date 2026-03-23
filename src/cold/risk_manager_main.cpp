// TASK_45: Risk Manager 실행 파일

#include "arbitrage/cold/risk_manager_process.hpp"
#include "arbitrage/common/logger.hpp"

int main(int argc, char* argv[]) {
    arbitrage::Logger::init("logs");

    auto config = arbitrage::RiskManagerProcess::parse_args(argc, argv);
    arbitrage::RiskManagerProcess process(config);
    return process.run();
}
