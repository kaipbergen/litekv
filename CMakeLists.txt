cmake_minimum_required(VERSION 3.16)
project(LiteKV VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_BUILD_TYPE Release)

set(CMAKE_CXX_FLAGS_RELEASE "-O2")

include_directories(include)

add_executable(litekv
    src/main.cpp
    src/server.cpp
    src/storage.cpp
    src/parser.cpp
)

target_compile_options(litekv PRIVATE -Wall -Wextra)

enable_testing()

add_executable(litekv_tests
    tests/test_storage.cpp
    tests/test_parser.cpp
    src/storage.cpp
    src/parser.cpp
)
target_include_directories(litekv_tests PRIVATE tests)

add_test(NAME litekv_tests COMMAND litekv_tests)