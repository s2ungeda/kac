# 테스트 설정
enable_testing()

# GTest 테스트 (옵션)
if(GTest_FOUND)
    add_executable(test_all
        # 테스트 파일들이 추가될 예정
    )

    target_link_libraries(test_all
        PRIVATE
            common
            GTest::gtest
            GTest::gtest_main
    )

    # CTest 등록
    add_test(NAME test_all COMMAND test_all)
endif()

# Integration Tests (TASK_29)
add_executable(integration_test
    integration/main.cpp
    integration/integration_test.cpp
)

target_include_directories(integration_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/integration
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(integration_test
    PRIVATE
        common
        strategy
        executor
        infra
        ops
        Threads::Threads
)

# Boost 링크
if(Boost_FOUND)
    target_link_libraries(integration_test PRIVATE ${Boost_LIBRARIES})
endif()

# OpenSSL 링크
if(OpenSSL_FOUND)
    target_link_libraries(integration_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# CURL 링크
if(CURL_FOUND)
    target_link_libraries(integration_test PRIVATE ${CURL_LIBRARIES})
endif()

# CTest 등록
add_test(NAME integration_test COMMAND integration_test)
