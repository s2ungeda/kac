# Cold Path 프로세스 라이브러리 (TASK_44~46)

add_library(cold
    order_manager_process.cpp
    risk_manager_process.cpp
    monitor_process.cpp
)

target_link_libraries(cold
    PUBLIC
        common
        ipc
        executor
        exchange_order
        strategy
        infra
        ops
        Threads::Threads
)

# Boost 링크
if(Boost_FOUND)
    target_link_libraries(cold PUBLIC ${Boost_LIBRARIES})
endif()

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(cold PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# CURL 링크
if(CURL_FOUND)
    target_link_libraries(cold PUBLIC ${CURL_LIBRARIES})
endif()

# =============================================================================
# TASK_44: Order Manager
# =============================================================================
add_executable(order-manager order_manager_main.cpp)
target_link_libraries(order-manager PRIVATE cold)

# =============================================================================
# TASK_45: Risk Manager
# =============================================================================
add_executable(risk-manager risk_manager_main.cpp)
target_link_libraries(risk-manager PRIVATE cold)

# =============================================================================
# TASK_46: Monitor
# =============================================================================
add_executable(monitor monitor_main.cpp)
target_link_libraries(monitor PRIVATE cold)
