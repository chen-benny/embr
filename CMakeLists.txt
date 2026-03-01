cmake_minimum_required(VERSION 4.1)
project(embr)

set(CMAKE_CXX_STANDARD 23)

add_executable(embr main.cpp)

include(FetchContent)
FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_subdirectory(tests)