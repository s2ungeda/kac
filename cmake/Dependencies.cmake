# 의존성 찾기

# Threads
find_package(Threads REQUIRED)

# OpenSSL (필수)
find_package(OpenSSL REQUIRED)

# Boost (시스템에 설치된 버전 사용)
find_package(Boost COMPONENTS system)

# libcurl
find_package(CURL)

# SQLite3  
find_package(SQLite3)

# yaml-cpp
find_package(yaml-cpp)

# spdlog
find_package(spdlog)

# simdjson
find_package(simdjson)

# Google Test
find_package(GTest)

# msgpack (선택)
find_package(msgpack-cxx CONFIG QUIET)

# 전역 include 경로
if(Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIRS})
endif()

if(CURL_FOUND)
    include_directories(${CURL_INCLUDE_DIRS})
endif()

# 전역 링크 디렉토리
if(Boost_FOUND)
    link_directories(${Boost_LIBRARY_DIRS})
endif()