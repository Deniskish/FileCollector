cmake_minimum_required(VERSION 3.20)
project(FileCollector LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FC_SANITIZER "Sanitizer for tests: thread | address | none" none)

add_library(file_collector INTERFACE)
target_include_directories(file_collector INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# GoogleTest: системный, если установлен, иначе скачивается.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # MSVC
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

enable_testing()
add_executable(file_collector_tests tests/FileCollectorTest.cpp)
target_link_libraries(file_collector_tests PRIVATE file_collector GTest::gtest_main)

if(MSVC)
    target_compile_options(file_collector_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(file_collector_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(FC_SANITIZER STREQUAL "thread" OR FC_SANITIZER STREQUAL "address")
        target_compile_options(file_collector_tests PRIVATE -fsanitize=${FC_SANITIZER} -g)
        target_link_options(file_collector_tests PRIVATE -fsanitize=${FC_SANITIZER})
    endif()
endif()

include(GoogleTest)
gtest_discover_tests(file_collector_tests)
