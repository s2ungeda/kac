# 테스트 설정
enable_testing()

# 테스트 실행 파일
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