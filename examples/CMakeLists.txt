# WebSocket 예제
add_executable(websocket_example websocket_example.cpp)

target_link_libraries(websocket_example
    PRIVATE
        common
        exchange_websocket
        Threads::Threads
)

# Boost 링크
if(Boost_FOUND)
    target_link_libraries(websocket_example PRIVATE ${Boost_LIBRARIES})
endif()

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(websocket_example PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# Order API 예제
add_executable(order_example order_example.cpp)

target_link_libraries(order_example
    PRIVATE
        common
        exchange_order
        Threads::Threads
)

# CURL 링크
if(CURL_FOUND)
    target_link_libraries(order_example PRIVATE ${CURL_LIBRARIES})
endif()

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(order_example PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# FXRate 예제
add_executable(fxrate_example fxrate_example.cpp)

target_link_libraries(fxrate_example
    PRIVATE
        common
        Threads::Threads
)

# CURL 링크 (HTTP 통신에 필요)
if(CURL_FOUND)
    target_link_libraries(fxrate_example PRIVATE ${CURL_LIBRARIES})
endif()

# Premium Calculator 예제
add_executable(premium_example premium_example.cpp)

target_link_libraries(premium_example
    PRIVATE
        common
        strategy
        exchange_websocket
        Threads::Threads
)

# Boost 링크
if(Boost_FOUND)
    target_link_libraries(premium_example PRIVATE ${Boost_LIBRARIES})
endif()

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(premium_example PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# CURL 링크
if(CURL_FOUND)
    target_link_libraries(premium_example PRIVATE ${CURL_LIBRARIES})
endif()

# Test MEXC
add_executable(test_mexc ${CMAKE_SOURCE_DIR}/test_mexc.cpp)
target_link_libraries(test_mexc
    PRIVATE
        common
        exchange_websocket
        Threads::Threads
        ${Boost_LIBRARIES}
        OpenSSL::SSL 
        OpenSSL::Crypto
)

# Test Simple MEXC
add_executable(test_simple_mexc ${CMAKE_SOURCE_DIR}/test_simple_mexc.cpp)
target_link_libraries(test_simple_mexc
    PRIVATE
        ${Boost_LIBRARIES}
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
)

# Data Logger - 실시간 데이터 파일 저장
add_executable(data_logger data_logger.cpp)

target_link_libraries(data_logger
    PRIVATE
        common
        strategy
        exchange_websocket
        Threads::Threads
)

# Boost 링크
if(Boost_FOUND)
    target_link_libraries(data_logger PRIVATE ${Boost_LIBRARIES})
endif()

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(data_logger PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# CURL 링크
if(CURL_FOUND)
    target_link_libraries(data_logger PRIVATE ${CURL_LIBRARIES})
endif()

# Low Latency Infrastructure Test
add_executable(lowlatency_test lowlatency_test.cpp)

target_link_libraries(lowlatency_test
    PRIVATE
        common
        Threads::Threads
)

# Rate Limiter & JSON Parser Test
add_executable(rate_limiter_test rate_limiter_test.cpp)

target_link_libraries(rate_limiter_test
    PRIVATE
        common
        Threads::Threads
)

# Executor Test (TASK_08)
add_executable(executor_test executor_test.cpp)

target_link_libraries(executor_test
    PRIVATE
        common
        executor
        exchange_order
        Threads::Threads
)

# CURL 링크
if(CURL_FOUND)
    target_link_libraries(executor_test PRIVATE ${CURL_LIBRARIES})
endif()

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(executor_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# Transfer Test (TASK_09)
add_executable(transfer_test transfer_test.cpp)

target_link_libraries(transfer_test
    PRIVATE
        common
        executor
        exchange_order
        Threads::Threads
)

# CURL 링크
if(CURL_FOUND)
    target_link_libraries(transfer_test PRIVATE ${CURL_LIBRARIES})
endif()

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(transfer_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# OrderBook Analyzer Test (TASK_10)
add_executable(orderbook_analyzer_test orderbook_analyzer_test.cpp)

target_link_libraries(orderbook_analyzer_test
    PRIVATE
        common
        strategy
        Threads::Threads
)

# Fee Calculator Test (TASK_11)
add_executable(fee_calculator_test fee_calculator_test.cpp)

target_link_libraries(fee_calculator_test
    PRIVATE
        common
        Threads::Threads
)

# Secrets Manager Test (TASK_16)
add_executable(secrets_test secrets_test.cpp)

target_link_libraries(secrets_test
    PRIVATE
        common
        Threads::Threads
)

# OpenSSL 링크 (암호화에 필요)
if(OpenSSL_FOUND)
    target_link_libraries(secrets_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# Event Bus Test (TASK_19)
add_executable(event_bus_test event_bus_test.cpp)

target_link_libraries(event_bus_test
    PRIVATE
        common
        infra
        Threads::Threads
)

# Thread Manager Test (TASK_20)
add_executable(thread_manager_test thread_manager_test.cpp)

target_link_libraries(thread_manager_test
    PRIVATE
        common
        Threads::Threads
)

# Shutdown Manager Test (TASK_21)
add_executable(shutdown_test shutdown_test.cpp)

target_link_libraries(shutdown_test
    PRIVATE
        common
        infra
        Threads::Threads
)

# Health Check Test (TASK_22)
add_executable(health_check_test health_check_test.cpp)

target_link_libraries(health_check_test
    PRIVATE
        common
        infra
        Threads::Threads
)

# TCP Server Test (TASK_23)
add_executable(tcp_server_test tcp_server_test.cpp)

target_link_libraries(tcp_server_test
    PRIVATE
        common
        infra
        Threads::Threads
)

# Alert System Test (TASK_24)
add_executable(alert_test alert_test.cpp)

target_link_libraries(alert_test
    PRIVATE
        common
        infra
        ops
        Threads::Threads
)

# Daily Loss Limit Test (TASK_25)
add_executable(daily_limit_test daily_limit_test.cpp)

target_link_libraries(daily_limit_test
    PRIVATE
        common
        infra
        ops
        Threads::Threads
)

# Watchdog Test (TASK_26)
add_executable(watchdog_test watchdog_test.cpp)

target_link_libraries(watchdog_test
    PRIVATE
        common
        infra
        Threads::Threads
)

# CLI Test (TASK_27)
add_executable(cli_test cli_test.cpp ${CMAKE_SOURCE_DIR}/tools/cli/commands.cpp)

target_include_directories(cli_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/cli
)

target_link_libraries(cli_test
    PRIVATE
        Threads::Threads
)

# Trading Stats Test (TASK_28)
add_executable(trading_stats_test trading_stats_test.cpp)

target_link_libraries(trading_stats_test
    PRIVATE
        common
        infra
        ops
        Threads::Threads
)

# SHM SPSC Queue Test (TASK_36)
add_executable(shm_queue_test shm_queue_test.cpp)

target_link_libraries(shm_queue_test
    PRIVATE
        common
        ipc
        Threads::Threads
        rt
)

# ShmRingBuffer + ShmLatestValue 단위 테스트
add_executable(shm_ringbuffer_test shm_ringbuffer_test.cpp)

target_link_libraries(shm_ringbuffer_test
    PRIVATE
        common
        ipc
        Threads::Threads
        rt
)

# SHM Reader — Ticker + OrderBook 진단 도구
add_executable(shm_reader shm_reader.cpp)

target_link_libraries(shm_reader
    PRIVATE
        common
        ipc
        Threads::Threads
        rt
)

# Feeder Process Test (TASK_37)
add_executable(feeder_test feeder_test.cpp)

target_link_libraries(feeder_test
    PRIVATE
        common
        ipc
        feeder
        exchange_websocket
        Threads::Threads
        rt
)

# Boost 링크
if(Boost_FOUND)
    target_link_libraries(feeder_test PRIVATE ${Boost_LIBRARIES})
endif()

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(feeder_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()