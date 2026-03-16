/**
 * Integration Test Main (TASK_29)
 *
 * 통합 테스트 실행 프로그램
 */

#include "integration_test.hpp"

#include <iostream>
#include <string>

using namespace arbitrage::integration;

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options] [category]\n"
              << "\n"
              << "Options:\n"
              << "  --verbose, -v     Verbose output\n"
              << "  --stop-on-fail    Stop on first failure\n"
              << "  --skip-slow       Skip slow tests\n"
              << "  --help, -h        Show this help\n"
              << "\n"
              << "Categories:\n"
              << "  all               Run all tests (default)\n"
              << "  connectivity      API connectivity tests\n"
              << "  datafeed          Data feed tests\n"
              << "  premium           Premium calculation tests\n"
              << "  strategy          Strategy tests\n"
              << "  dryrun            Dry-run execution tests\n"
              << "  infra             Infrastructure tests\n"
              << "  performance       Performance tests\n"
              << "\n";
}

int main(int argc, char* argv[]) {
    IntegrationTestConfig config;
    std::string category = "all";

    // 명령줄 파싱
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--verbose" || arg == "-v") {
            config.verbose = true;
        } else if (arg == "--stop-on-fail") {
            config.stop_on_failure = true;
        } else if (arg == "--skip-slow") {
            config.skip_slow_tests = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg[0] != '-') {
            category = arg;
        }
    }

    std::cout << "\n========================================\n";
    std::cout << "  Kimchi Arbitrage Integration Tests\n";
    std::cout << "  (TASK_29)\n";
    std::cout << "========================================\n\n";

    IntegrationTestRunner runner(config);

    // 진행 상황 콜백
    runner.on_progress([&config](const std::string& name, TestStatus status) {
        if (!config.verbose && status == TestStatus::Running) {
            return;  // 실행 중 메시지 생략
        }

        const char* status_str = "???";
        switch (status) {
            case TestStatus::Running: status_str = "RUNNING"; break;
            case TestStatus::Passed:  status_str = "PASSED";  break;
            case TestStatus::Failed:  status_str = "FAILED";  break;
            case TestStatus::Skipped: status_str = "SKIPPED"; break;
            case TestStatus::Timeout: status_str = "TIMEOUT"; break;
            default: break;
        }

        std::cout << "  [" << status_str << "] " << name << std::endl;
    });

    // 테스트 등록
    runner.register_default_tests();

    // 테스트 실행
    TestSummary summary;

    if (category == "all") {
        summary = runner.run_all();
    } else if (category == "connectivity") {
        summary = runner.run_category(TestCategory::Connectivity);
    } else if (category == "datafeed") {
        summary = runner.run_category(TestCategory::DataFeed);
    } else if (category == "premium") {
        summary = runner.run_category(TestCategory::Premium);
    } else if (category == "strategy") {
        summary = runner.run_category(TestCategory::Strategy);
    } else if (category == "dryrun") {
        summary = runner.run_category(TestCategory::DryRun);
    } else if (category == "infra") {
        summary = runner.run_category(TestCategory::Infrastructure);
    } else if (category == "performance") {
        summary = runner.run_category(TestCategory::Performance);
    } else {
        std::cerr << "Unknown category: " << category << "\n";
        print_usage(argv[0]);
        return 1;
    }

    // 결과 출력
    runner.print_summary(summary);

    return summary.all_passed() ? 0 : 1;
}
