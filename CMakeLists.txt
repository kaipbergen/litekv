cmake_minimum_required(VERSION 3.16)
project(LiteKV VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

include_directories(include)

add_executable(litekv
    src/main.cpp
    src/server.cpp
    src/storage.cpp
    src/parser.cpp
)

target_compile_options(litekv PRIVATE -Wall -Wextra)

option(ENABLE_CLANG_TIDY "Run clang-tidy on litekv sources during build" OFF)
if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set_target_properties(litekv PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    else()
        message(WARNING "ENABLE_CLANG_TIDY is ON but clang-tidy was not found")
    endif()
endif()

enable_testing()

add_executable(litekv_tests
    tests/test_storage.cpp
    tests/test_parser.cpp
    src/storage.cpp
    src/parser.cpp
)
target_include_directories(litekv_tests PRIVATE tests)

add_test(NAME litekv_tests COMMAND litekv_tests)

option(ENABLE_COVERAGE "Build litekv_tests with gcov/lcov code coverage instrumentation" OFF)
if(ENABLE_COVERAGE)
    target_compile_options(litekv_tests PRIVATE --coverage -O0 -g)
    target_link_options(litekv_tests PRIVATE --coverage)

    add_custom_target(coverage
        COMMAND $<TARGET_FILE:litekv_tests>
        COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/coverage.sh ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS litekv_tests
        COMMENT "Running tests and generating code coverage report"
    )
endif()