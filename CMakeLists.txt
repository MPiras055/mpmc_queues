cmake_minimum_required(VERSION 3.20)
project(MPMCQueue LANGUAGES CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# === Include all subdirectories in include/ ===
file(GLOB_RECURSE
    ALL_INCLUDE_DIRS
    LIST_DIRECTORIES true
    "${PROJECT_SOURCE_DIR}/include/*"
)

set(INCLUDE_DIRS "")
foreach(dir ${ALL_INCLUDE_DIRS})
    if(IS_DIRECTORY ${dir})
        list(APPEND INCLUDE_DIRS ${dir})
    endif()
endforeach()
include_directories(${INCLUDE_DIRS})

# === Additional Work utilities
add_executable(timeTicks "src/util/time/timeTicks.cpp")
target_compile_options(timeTicks PUBLIC -O3 -DNDEBUG -funroll-loops -fno-rtti)

# === Test Stub
add_executable(testStub "src/sim/test_stub_autogen.cpp")
target_compile_options(
    testStub PUBLIC -O3 -DNDEBUG -funroll-loops -fno-rtti -march=native)
target_precompile_headers(testStub PUBLIC "include/bench/benchmark.hpp")


# === GoogleTest ===
set(GTEST_DIR ${PROJECT_SOURCE_DIR}/cmake/extern/googletest)
add_subdirectory(${GTEST_DIR})  # <-- This is the proper way to include gtest

# === Unit Tests ===
file(GLOB UNIT_TEST_SOURCES "src/test/unit/*.cpp")

foreach(test_src ${UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)

    add_executable(${test_name} ${test_src})

    target_compile_options(${test_name} PRIVATE
        -g -O3 -march=native -funroll-loops -fno-rtti)

    target_link_libraries(${test_name} PRIVATE gtest_main)

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# === Integration Tests ===
file(GLOB INTEGRATION_TEST_SOURCES "src/test/integration/*.cpp")

foreach(test_src ${INTEGRATION_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)

    add_executable(${test_name} ${test_src})

    target_compile_options(${test_name} PRIVATE
        -O3 -march=native -funroll-loops -fno-rtti)

    target_link_libraries(${test_name} PRIVATE gtest_main)

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
