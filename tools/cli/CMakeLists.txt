# CLI Tool (TASK_27)

# CLI 라이브러리
add_library(cli_lib STATIC
    commands.cpp
)

target_include_directories(cli_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(cli_lib
    PUBLIC
        Threads::Threads
)

# CLI 실행 파일
add_executable(arbitrage-cli
    main.cpp
)

target_link_libraries(arbitrage-cli
    PRIVATE
        cli_lib
        Threads::Threads
)

# 출력 디렉토리 설정
set_target_properties(arbitrage-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
