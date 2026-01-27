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