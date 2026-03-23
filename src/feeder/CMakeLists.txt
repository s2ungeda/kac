# Feeder 라이브러리 (TASK_37: FeederProcess)
add_library(feeder
    feeder_process.cpp
)

target_link_libraries(feeder
    PUBLIC
        common
        ipc
        exchange_websocket
        Threads::Threads
)

# Boost 링크
if(Boost_FOUND)
    target_link_libraries(feeder PUBLIC ${Boost_LIBRARIES})
endif()

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(feeder PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# =============================================================================
# TASK_38: 4개 Feeder 실행 파일
# =============================================================================

# Upbit Feeder
add_executable(upbit-feeder upbit_main.cpp)
target_link_libraries(upbit-feeder PRIVATE feeder)

# Bithumb Feeder
add_executable(bithumb-feeder bithumb_main.cpp)
target_link_libraries(bithumb-feeder PRIVATE feeder)

# Binance Feeder
add_executable(binance-feeder binance_main.cpp)
target_link_libraries(binance-feeder PRIVATE feeder)

# MEXC Feeder
add_executable(mexc-feeder mexc_main.cpp)
target_link_libraries(mexc-feeder PRIVATE feeder)
