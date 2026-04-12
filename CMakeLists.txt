cmake_minimum_required(VERSION 3.25)
project(embr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED liburing)

# Transport lib
add_library(transport
        src/transport/tcp_server.cpp
        src/transport/tcp_client.cpp
        src/transport/tcp_transport.cpp
        src/transport/udp_transport.cpp
        src/transport/udp_data_client.cpp
        src/transport/udp_data_server.cpp
        src/util/io_uring_ctx.cpp
)
target_include_directories(transport PUBLIC src)
target_include_directories(transport PUBLIC ${URING_INCLUDE_DIRS})
target_link_libraries(transport PUBLIC ${URING_LIBRARIES})

# Core lib
add_library(core
        src/core/protocol.cpp
        src/core/push.cpp
        src/core/pull.cpp
        src/core/hash.cpp
)
target_link_libraries(core PUBLIC transport OpenSSL::Crypto)
target_include_directories(core PUBLIC src)

# Main bin
add_executable(embr src/main.cpp)
target_link_libraries(embr PRIVATE core)

# Test
include(FetchContent)
FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

add_executable(test
        tests/test_protocol.cpp
        tests/test_tcp.cpp
)
target_link_libraries(test PRIVATE core GTest::gtest_main)
find_package(Threads REQUIRED)
target_link_directories(test PRIVATE Threads::Threads)

include(GoogleTest)
gtest_discover_tests(test)