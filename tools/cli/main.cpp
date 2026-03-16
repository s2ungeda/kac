/**
 * Kimchi Arbitrage CLI Tool (TASK_27)
 *
 * 시스템 관리 및 디버깅용 명령줄 도구
 * - 상태 조회
 * - 수동 주문
 * - 킬스위치 제어
 * - 설정 변경
 */

#include "commands.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace arbitrage::cli;

/**
 * 인자 파싱
 */
bool parse_args(int argc, char* argv[], CLIConfig& config,
                std::string& command, std::vector<std::string>& args)
{
    int i = 1;

    // 옵션 파싱
    while (i < argc && argv[i][0] == '-') {
        std::string opt = argv[i];

        if (opt == "-h" || opt == "--host") {
            if (++i >= argc) {
                std::cerr << "Error: --host requires argument\n";
                return false;
            }
            config.server_host = argv[i];
        }
        else if (opt == "-p" || opt == "--port") {
            if (++i >= argc) {
                std::cerr << "Error: --port requires argument\n";
                return false;
            }
            config.server_port = std::stoi(argv[i]);
        }
        else if (opt == "-t" || opt == "--token") {
            if (++i >= argc) {
                std::cerr << "Error: --token requires argument\n";
                return false;
            }
            config.auth_token = argv[i];
        }
        else if (opt == "-v" || opt == "--verbose") {
            config.verbose = true;
        }
        else if (opt == "--no-color") {
            config.color_output = false;
        }
        else if (opt == "--version") {
            print_version();
            return false;
        }
        else if (opt == "--help") {
            print_usage(argv[0]);
            return false;
        }
        else {
            std::cerr << "Unknown option: " << opt << "\n";
            return false;
        }
        ++i;
    }

    // 명령어 파싱
    if (i >= argc) {
        std::cerr << "Error: No command specified\n";
        print_usage(argv[0]);
        return false;
    }

    command = argv[i++];

    // 나머지 인자
    while (i < argc) {
        args.push_back(argv[i++]);
    }

    return true;
}

/**
 * 명령어 실행
 */
int execute_command(CLI& cli, const std::string& command,
                    const std::vector<std::string>& args)
{
    // 조회 명령
    if (command == "status") {
        auto result = cli.status();
        if (result) {
            cli.print_status(*result);
            return 0;
        }
        std::cerr << "Error: " << cli.last_error() << "\n";
        return 1;
    }

    if (command == "premium") {
        auto result = cli.premium();
        if (result) {
            cli.print_premium(*result);
            return 0;
        }
        std::cerr << "Error: " << cli.last_error() << "\n";
        return 1;
    }

    if (command == "balance") {
        auto result = cli.balance();
        if (result) {
            cli.print_balance(*result);
            return 0;
        }
        std::cerr << "Error: " << cli.last_error() << "\n";
        return 1;
    }

    if (command == "history") {
        int count = 10;
        if (!args.empty()) {
            count = std::stoi(args[0]);
        }
        auto result = cli.history(count);
        if (result) {
            cli.print_history(*result);
            return 0;
        }
        std::cerr << "Error: " << cli.last_error() << "\n";
        return 1;
    }

    if (command == "health") {
        auto result = cli.health();
        if (result) {
            cli.print_health(*result);
            return 0;
        }
        std::cerr << "Error: " << cli.last_error() << "\n";
        return 1;
    }

    // 제어 명령
    if (command == "order") {
        if (args.size() < 3) {
            std::cerr << "Usage: order <exchange> <side> <qty> [price]\n";
            return 1;
        }
        std::string exchange = args[0];
        std::string side = args[1];
        double qty = std::stod(args[2]);
        double price = args.size() > 3 ? std::stod(args[3]) : 0.0;

        auto result = cli.order(exchange, side, qty, price);
        cli.print_response(result);
        return result.success ? 0 : 1;
    }

    if (command == "cancel") {
        if (args.empty()) {
            std::cerr << "Usage: cancel <order_id>\n";
            return 1;
        }
        auto result = cli.cancel(args[0]);
        cli.print_response(result);
        return result.success ? 0 : 1;
    }

    if (command == "kill") {
        std::string reason = args.empty() ? "Manual CLI kill" : args[0];
        auto result = cli.kill(reason);
        cli.print_response(result);
        return result.success ? 0 : 1;
    }

    if (command == "resume") {
        auto result = cli.resume();
        cli.print_response(result);
        return result.success ? 0 : 1;
    }

    if (command == "start") {
        std::string strategy = args.empty() ? "" : args[0];
        auto result = cli.start_strategy(strategy);
        cli.print_response(result);
        return result.success ? 0 : 1;
    }

    if (command == "stop") {
        std::string strategy = args.empty() ? "" : args[0];
        auto result = cli.stop_strategy(strategy);
        cli.print_response(result);
        return result.success ? 0 : 1;
    }

    if (command == "config") {
        if (args.empty()) {
            std::cerr << "Usage: config <key> [value]\n";
            return 1;
        }
        if (args.size() == 1) {
            // 조회
            auto result = cli.config_get(args[0]);
            if (result) {
                std::cout << args[0] << " = " << *result << "\n";
                return 0;
            }
            std::cerr << "Config not found: " << args[0] << "\n";
            return 1;
        } else {
            // 설정
            auto result = cli.config_set(args[0], args[1]);
            cli.print_response(result);
            return result.success ? 0 : 1;
        }
    }

    // 알 수 없는 명령
    std::cerr << "Unknown command: " << command << "\n";
    print_usage("arbitrage-cli");
    return 1;
}

int main(int argc, char* argv[]) {
    CLIConfig config;
    std::string command;
    std::vector<std::string> args;

    if (!parse_args(argc, argv, config, command, args)) {
        return 1;
    }

    CLI cli(config);

    return execute_command(cli, command, args);
}
