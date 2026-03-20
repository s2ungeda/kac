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
