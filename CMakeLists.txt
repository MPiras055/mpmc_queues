cmake_minimum_required(VERSION 3.20)
project(MPMCQueue LANGUAGES CXX)

# Use NEW behavior for CMP0135 to fix FetchContent timestamp warning
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

# Get all subdirectories under include/
set(GTEST_DIR
    ${PROJECT_SOURCE_DIR}/cmake/extern/googletest
)

file(
    GLOB_RECURSE ALL_INCLUDE_DIRS LIST_DIRECTORIES
    true "${PROJECT_SOURCE_DIR}/include/*")

# Filter to keep only directories
set(INCLUDE_DIRS "")
foreach(dir ${ALL_INCLUDE_DIRS})
    if(IS_DIRECTORY ${dir})
        list(APPEND INCLUDE_DIRS ${dir})
    endif()
endforeach()

# Add all directories to include path
include_directories(${INCLUDE_DIRS})

# Fetch GoogleTest
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR ${GTEST_DIR}
)

FetchContent_MakeAvailable(googletest)

# === Unit Tests ===
file(GLOB UNIT_TEST_SOURCES "src/test/unit/*.cpp")

foreach(test_src ${UNIT_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)

    # Create executable
    add_executable(${test_name} ${test_src})

    # Compile with address sanitizer
    TARGET_COMPILE_OPTIONS(${test_name} PUBLIC
     -O3
     -march=native
     -funroll-loops
     -fno-rtti
     -DNDEBUG
    )

    # link gtest and fsanitize
    target_link_libraries(${test_name} gtest_main)
    # TARGET_COMPILE_OPTIONS(${test_name} PUBLIC -fsanitize=address -g -O3)
    # TARGET_LINK_LIBRARIES(${test_name} PUBLIC gtest_main -fsanitize=address)

    # Register with CTest
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# === Integration Tests ===
file(GLOB INTEGRATION_TEST_SOURCES "src/test/integration/*.cpp")
foreach(test_src ${INTEGRATION_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    ADD_EXECUTABLE(${test_name} ${test_src})
    target_link_libraries(${test_name} gtest_main)
    target_compile_options(${test_name} PUBLIC
        -O3
        -march=native
        -funroll-loops
        -fno-rtti)
    target_link_options(${test_name} PUBLIC gtest_main)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
