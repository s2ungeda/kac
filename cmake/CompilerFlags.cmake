# 컴파일러별 플래그 설정

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # GCC / Clang
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        -Werror=return-type
        -fPIC
    )
    
    # Release 최적화
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
    
    # Debug 설정
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")
    
elseif(MSVC)
    # Visual Studio
    add_compile_options(
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
    )
    
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi")
endif()

# 링크 타임 최적화 (Release)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_SUPPORTED)
    if(IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()