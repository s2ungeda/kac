# 공통 라이브러리
add_library(common
    common/config.cpp
    common/logger.cpp
    common/http_client.cpp
    common/crypto.cpp
    common/fxrate.cpp
    common/fee_calculator.cpp
    common/config_watcher.cpp
    common/secrets.cpp
    common/account_manager.cpp
    common/symbol_master.cpp
    common/thread_manager.cpp
    common/numa_allocator.cpp
)

# Exchange WebSocket 라이브러리
add_library(exchange_websocket
    exchange/websocket_base.cpp
    exchange/upbit/websocket.cpp
    exchange/binance/websocket.cpp
    exchange/bithumb/websocket.cpp
    exchange/mexc/websocket.cpp
    exchange/mexc/protobuf_parser.cpp
)

# Exchange Order 라이브러리
add_library(exchange_order
    exchange/order_base.cpp
    exchange/upbit/order.cpp
    exchange/binance/order.cpp
)

# Strategy 라이브러리
add_library(strategy
    strategy/premium_calc.cpp
    strategy/liquidity_metrics.cpp
    strategy/slippage_model.cpp
    strategy/orderbook_analyzer.cpp
    strategy/risk_model.cpp
    strategy/decision_engine.cpp
    strategy/strategy_executor.cpp
    strategy/strategies/basic_arb_strategy.cpp
)

# Executor 라이브러리
add_library(executor
    executor/dual_order.cpp
    executor/recovery.cpp
    executor/transfer.cpp
)

# Infra 라이브러리 (TASK_19, TASK_21, TASK_22, TASK_23, TASK_26)
add_library(infra
    infra/event_bus.cpp
    infra/shutdown.cpp
    infra/health_check.cpp
    infra/tcp_server.cpp
    infra/watchdog_client.cpp
    infra/watchdog.cpp
)

# IPC 라이브러리 (TASK_36: Shared Memory SPSC Queue)
add_subdirectory(ipc)

# Feeder 라이브러리 (TASK_37: FeederProcess)
add_subdirectory(feeder)

# Ops 라이브러리 (TASK_24, TASK_25, TASK_28)
add_library(ops
    ops/alert.cpp
    ops/daily_limit.cpp
    ops/trading_stats.cpp
)

# 공통 라이브러리 링크
target_link_libraries(common
    PUBLIC
        Threads::Threads
)

# spdlog와 yaml-cpp가 있으면 링크
if(TARGET spdlog::spdlog)
    target_link_libraries(common PUBLIC spdlog::spdlog)
endif()

if(TARGET yaml-cpp)
    target_link_libraries(common PUBLIC yaml-cpp)
endif()

# CURL 링크 (HTTP client에 필요)
if(CURL_FOUND)
    target_link_libraries(common PUBLIC ${CURL_LIBRARIES})
endif()

# OpenSSL 링크 (crypto에 필요)
if(OpenSSL_FOUND)
    target_link_libraries(common PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# Exchange WebSocket 라이브러리 링크
target_link_libraries(exchange_websocket
    PUBLIC
        common
        Threads::Threads
)

# Boost 링크 (WebSocket에 필요)
if(Boost_FOUND)
    target_link_libraries(exchange_websocket PUBLIC ${Boost_LIBRARIES})
endif()

# OpenSSL 링크 (SSL WebSocket에 필요)
if(OpenSSL_FOUND)
    target_link_libraries(exchange_websocket PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# Exchange Order 라이브러리 링크
target_link_libraries(exchange_order
    PUBLIC
        common
        Threads::Threads
)

# CURL 링크 (Order API에 필요)
if(CURL_FOUND)
    target_link_libraries(exchange_order PUBLIC ${CURL_LIBRARIES})
endif()

# OpenSSL 링크 (HMAC에 필요)
if(OpenSSL_FOUND)
    target_link_libraries(exchange_order PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# Strategy 라이브러리 링크
target_link_libraries(strategy
    PUBLIC
        common
        Threads::Threads
)

# Executor 라이브러리 링크
target_link_libraries(executor
    PUBLIC
        common
        exchange_order
        Threads::Threads
)

# Infra 라이브러리 링크 (TASK_19)
target_link_libraries(infra
    PUBLIC
        common
        Threads::Threads
)

# Ops 라이브러리 링크 (TASK_24)
target_link_libraries(ops
    PUBLIC
        common
        infra
        Threads::Threads
)

# 메인 실행 파일
add_executable(arbitrage
    main.cpp
)

target_link_libraries(arbitrage
    PRIVATE
        common
        strategy
        executor
        infra
        ops
        exchange_websocket
        exchange_order
        Threads::Threads
)

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(arbitrage PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# Boost 링크
if(Boost_FOUND)
    target_link_libraries(arbitrage PRIVATE ${Boost_LIBRARIES})
endif()

# CURL 링크
if(CURL_FOUND)
    target_link_libraries(arbitrage PRIVATE ${CURL_LIBRARIES})
endif()

# SQLite3 링크
if(SQLite3_FOUND)
    target_link_libraries(arbitrage PRIVATE SQLite::SQLite3)
endif()