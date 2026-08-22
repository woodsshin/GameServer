cmake_minimum_required(VERSION 3.16)
project(GameServer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_compile_options(-Wall -Wextra -pthread)

find_package(Threads REQUIRED)

# MySQL 클라이언트 라이브러리 탐색
find_path(MYSQL_INCLUDE_DIR mysql.h PATH_SUFFIXES mysql)
find_library(MYSQL_LIB NAMES mysqlclient)

# OpenSSL (비밀번호 해싱용)
find_package(OpenSSL REQUIRED)

if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIB)
    message(FATAL_ERROR "libmysqlclient을 찾을 수 없습니다. libmysqlclient-dev(또는 mysql-devel)를 설치하세요.")
endif()

include_directories(${MYSQL_INCLUDE_DIR})
include_directories(include)

add_executable(game_server
    src/main.cpp
    src/GameServer.cpp
    src/NetServer.cpp
)

target_link_libraries(game_server
    PRIVATE
    Threads::Threads
    ${MYSQL_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
)

# 테스트 클라이언트 (선택 빌드)
add_executable(test_client
    src/TestClient.cpp
)
target_link_libraries(test_client PRIVATE Threads::Threads)
