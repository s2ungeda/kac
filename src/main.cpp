/**
 * Kimchi Arbitrage System - Main Entry Point
 *
 * Architecture: Single-process Hot/Cold Thread separation
 * - Hot Thread: WebSocket -> SPSC -> Premium -> Decision -> Order Queue (TASK_31)
 * - Cold Threads: Logger, Alert, Stats, Health, Display, FX Rate
 *
 * Phase 8: TASK_30 (Skeleton) + TASK_31 (Hot/Cold Threads)
 * Phase 9: TASK_39 (SHM Consumer) - --engine mode
 *
 * Refactored: main.cpp is now a thin wrapper around Application class.
 */

#include "arbitrage/app/application.hpp"

#include <string>

using namespace arbitrage;

// =============================================================================
// Command-line argument parser
// =============================================================================
AppOptions parse_args(int argc, char* argv[]) {
    AppOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dry-run")             opts.dry_run = true;
        else if (arg == "--verbose")        opts.verbose = true;
        else if (arg == "--standalone")     opts.mode = RunMode::Standalone;
        else if (arg == "--engine")         opts.mode = RunMode::Engine;
        else if (arg == "--config-stdin")   opts.config_from_stdin = true;
        else if (arg == "--config" && i + 1 < argc) opts.config_path = argv[++i];
        else if (arg[0] != '-')             opts.config_path = arg;
    }
    return opts;
}

// =============================================================================
// main()
// =============================================================================
int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);
    Application app(opts);
    return app.run();
}
