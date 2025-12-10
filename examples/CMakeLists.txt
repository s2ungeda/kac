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