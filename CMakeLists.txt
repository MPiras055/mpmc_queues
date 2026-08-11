cmake_minimum_required(VERSION 3.20)

# ============================================================
# Toolchain selection -- must happen before project()
# ============================================================
# Default to clang when it is installed, compile_commands.json records the same
# compiler a clangd-based editor. Mixing them (database says GCC, editor runs
# clangd) makes clangd guess at system include paths from GCC's driver,
# common source of phantom "file not found" diagnostics.
#
# Override with any of:
#   cmake -DUSE_CLANG=OFF ...            (use the system default, normally GCC)
#   cmake -DCMAKE_CXX_COMPILER=g++ ...   (explicit)
#   CXX=g++ cmake ...                    (environment)
option(USE_CLANG "Prefer clang/clang++ when available" ON)

if(USE_CLANG AND NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(MPMC_CLANGXX NAMES clang++)
    find_program(MPMC_CLANGC NAMES clang)
    if(MPMC_CLANGXX AND MPMC_CLANGC)
        set(CMAKE_CXX_COMPILER "${MPMC_CLANGXX}" CACHE FILEPATH "C++ compiler" FORCE)
        set(CMAKE_C_COMPILER "${MPMC_CLANGC}" CACHE FILEPATH "C compiler" FORCE)
    endif()
endif()

project(MPMCQueue LANGUAGES C CXX)

# ============================================================
# Global configuration
# ============================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# compile_commands.json, for clangd and friends. FORCE so it holds even if a parent
# scope or a stale cache says otherwise.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Emit compile_commands.json" FORCE)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SANITIZE_ADDR      "Build tests with AddressSanitizer" ON)
option(SANITIZE_THREAD    "Build tests with ThreadSanitizer"  OFF)
option(BUILD_BENCHMARK    "Build the throughput benchmark"    ON)
option(BUILD_TESTING_MPMC "Build the unit tests"              ON)
option(NATIVE_ARCH        "Optimise for the building machine" ON)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# ============================================================
# Compile options
# ============================================================
# -mcx16 enables the 16-byte CAS that VyukovDCAS needs on x86_64; without it GCC will
# not emit cmpxchg16b and that comparator fails to build.
add_library(project_flags INTERFACE)
target_compile_features(project_flags INTERFACE cxx_std_20)
target_compile_options(project_flags INTERFACE
    -Wall -Wextra
    $<$<CXX_COMPILER_ID:GNU,Clang>:-mcx16>
)

add_library(project_opt INTERFACE)
target_compile_options(project_opt INTERFACE
    -O3 -funroll-loops -fno-rtti
    $<$<BOOL:${NATIVE_ARCH}>:-march=native>
)

# LTO is spelled differently per compiler: -flto=auto is a GCC extension, clang wants
# ThinLTO. Getting this wrong is a hard error, not a warning.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(MPMC_LTO_FLAG -flto=auto)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(MPMC_LTO_FLAG -flto=thin)
else()
    set(MPMC_LTO_FLAG "")
endif()

add_library(project_includes INTERFACE)
target_include_directories(project_includes INTERFACE ${PROJECT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)

# Convenience bundle: headers + warnings + threads, with no opinion on optimisation.
add_library(mpmc INTERFACE)
target_link_libraries(mpmc INTERFACE project_includes project_flags Threads::Threads atomic)

# ============================================================
# Topology generation (configure time)
# ============================================================
# Only the benchmark pins threads, so only the benchmark needs this. A missing or failing
# generator is a warning rather than a hard error -- previously it was FATAL_ERROR, which
# meant an unrelated Python problem blocked configuring the tests too.
set(TOPOLOGY_OUTPUT ${CMAKE_BINARY_DIR}/sys.topo)
set(TOPOLOGY_SCRIPT ${CMAKE_SOURCE_DIR}/python/mpmc_bench/topology/generator.py)

if(BUILD_BENCHMARK)
    # guard is on BUILD_BENCHMARK alone. Testing EXISTS here too would mean that
    # moving script skips generation *silently*, leaving a stale sys.topo
    # and a quietly broken `benchmark ... pin`.
    if(NOT EXISTS ${TOPOLOGY_SCRIPT})
        message(WARNING "Topology generator not found at ${TOPOLOGY_SCRIPT}; "
                        "benchmark 'pin' will be unavailable")
    else()
        find_package(Python3 COMPONENTS Interpreter)
        if(Python3_FOUND)
            execute_process(
                COMMAND 
                  ${Python3_EXECUTABLE} 
                  ${TOPOLOGY_SCRIPT} 
                  --pin_cluster ${TOPOLOGY_OUTPUT}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                RESULT_VARIABLE topo_result
                OUTPUT_QUIET ERROR_QUIET
            )
            if(topo_result EQUAL 0)
                message(STATUS "Topology written to ${TOPOLOGY_OUTPUT}")
            else()
                message(WARNING "Topology generation failed; 
                benchmark 'pin' will be unavailable")
            endif()
        endif()
    endif()
endif()

# ============================================================
# Third-party
# ============================================================
# GoogleTest, from whichever of three places is available -- in that order:
#
#   1. -DGTEST_DIR=/path/to/tree   an explicit local checkout; nothing is downloaded
#   2. cmake/extern/googletest     a checkout sitting in-tree, the historical layout
#   3. FetchContent                cloned from GTEST_REPO at GTEST_TAG, cached in the
#                                  build directory
#
# The point of (3) is that a fresh machine needs no manual step: `cmake -S . -B build` is
# enough. The point of keeping (1) and (2) is that a machine without network still builds
# from a tree copied by hand.
set(GTEST_DIR "" CACHE PATH "Local GoogleTest checkout; empty to search then fetch")
set(GTEST_REPO "https://github.com/MPiras055/googletest-minimal.git"
    CACHE STRING "Git remote used when no local GoogleTest is found")
set(GTEST_TAG "main" CACHE STRING "Ref to fetch from GTEST_REPO")

if(BUILD_TESTING_MPMC)
    enable_testing()

    if(GTEST_DIR AND EXISTS "${GTEST_DIR}/CMakeLists.txt")
        message(STATUS "GoogleTest: local tree at ${GTEST_DIR}")
        add_subdirectory(${GTEST_DIR} ${CMAKE_BINARY_DIR}/googletest-build EXCLUDE_FROM_ALL)
    elseif(EXISTS "${PROJECT_SOURCE_DIR}/cmake/extern/googletest/CMakeLists.txt")
        message(STATUS "GoogleTest: in-tree at cmake/extern/googletest")
        add_subdirectory(${PROJECT_SOURCE_DIR}/cmake/extern/googletest EXCLUDE_FROM_ALL)
    else()
        message(STATUS "GoogleTest: fetching ${GTEST_REPO}@${GTEST_TAG}")
        include(FetchContent)
        FetchContent_Declare(googletest
            GIT_REPOSITORY ${GTEST_REPO}
            GIT_TAG        ${GTEST_TAG}
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

# ============================================================
# Utilities
# ============================================================
add_executable(timeTicks src/util/time/timeTicks.cpp)
target_link_libraries(timeTicks PRIVATE mpmc project_opt)
target_compile_definitions(timeTicks PRIVATE NDEBUG)

# ============================================================
# Benchmark
# ============================================================
# binary covering every registered implementation, selected by name at startup
# through a compile-time fold over the registry (include/registry/Registry.hpp).
if(BUILD_BENCHMARK)
    add_executable(benchmark src/bench/main.cpp)
    target_link_libraries(benchmark PRIVATE mpmc project_opt)
    if(MPMC_LTO_FLAG)
        target_compile_options(benchmark PRIVATE ${MPMC_LTO_FLAG})
        target_link_options(benchmark PRIVATE ${MPMC_LTO_FLAG})
    endif()
    target_compile_definitions(
    benchmark 
    PRIVATE CORE_TOPOLOGY="${TOPOLOGY_OUTPUT}")
endif()

# ============================================================
# Unit tests
# ============================================================
if(BUILD_TESTING_MPMC)
    set(UNIT_TESTS
      RegistryConformanceTest # contract + sequential behaviour, every registry entry
        SegmentLifecycleTest      # close/reopen/link, per segment type, directly
        TaggingTest               # cell tagging policies
        AdmissionTest             # bound enforcement, per admission policy
        MemoryLayoutTest          # single-block layout arithmetic
        PoolReclamationTest       # the pooled source's epoch machine, deterministically
        ConcurrencyTest           # MPMC loss/duplication/ordering, several thread shapes
        ThreadRegistryTest        # the lock-free thread registry both sources are built on
        ThreadPinnerTest          # core placement rule and topology parsing
        ProxyAccountingTest       # size() across threads that join and leave
        BucketTest                # the phased and cache index buckets
    )

    foreach(test_name IN LISTS UNIT_TESTS)
        add_executable(${test_name} src/test/unit/${test_name}.cpp)
        # -O1 -g: optimised enough to shake races loose, still debuggable.
        target_compile_options(${test_name} PRIVATE -O1 -g)
        target_link_libraries(${test_name} PRIVATE mpmc gtest_main)

        # Suppress the type-parameter trailer on typed-test output.
        #
        # A registry entry's real type is its fully expanded template-id -- a pooled one runs
        # to several hundred characters and names the same segment three times -- and gtest
        # appends it to every typed-test line as `, where TypeParam = ...`. It carries no
        # information the test's own name does not, now that registry::TestNames labels the
        # cases (`Mpmc/mem_mutex` rather than `Mpmc/29`).
        #
        # GTEST_HAS_RTTI is gtest's documented knob for this (gtest-port.h): with it off,
        # internal::GetTypeName<T>() returns the literal "<type>" instead of demangling
        # typeid(T). Scoped to the test executables, and safe here because neither this
        # project nor its tests use dynamic_cast or typeid -- the whole design is deliberately
        # free of vtable polymorphism.
        target_compile_definitions(${test_name} PRIVATE GTEST_HAS_RTTI=0)

        if(SANITIZE_ADDR)
            target_compile_options(${test_name} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${test_name} PRIVATE -fsanitize=address)
        endif()
        if(SANITIZE_THREAD)
            target_compile_options(${test_name} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
            target_link_options(${test_name} PRIVATE -fsanitize=thread)
        endif()

        add_test(NAME ${test_name} COMMAND ${test_name})
        # The concurrency suite runs every implementation several times over.
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 900)
    endforeach()
endif()

# ============================================================
# Editor integration (.clangd)
# ============================================================
# compile_commands.json never contains headers, only translation units. clangd therefore
# infers a command for each header from the nearest indexed file -- which in this layout
# is a googletest source, whose flags do not include this project's include directory.
# The generated .clangd supplies those flags explicitly. See cmake/clangd.in.
set(CLANGD_DB_DIR "${CMAKE_BINARY_DIR}")
set(CLANGD_GTEST_INCLUDE "${PROJECT_SOURCE_DIR}/cmake/extern/googletest/googletest/include")
configure_file(
    ${PROJECT_SOURCE_DIR}/cmake/clangd.in
    ${PROJECT_SOURCE_DIR}/.clangd
    @ONLY
)

# A compile_commands.json at the project root, where editors look by default.
if(EXISTS "${CMAKE_BINARY_DIR}/compile_commands.json"
   AND NOT EXISTS "${PROJECT_SOURCE_DIR}/compile_commands.json")
    file(CREATE_LINK
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${PROJECT_SOURCE_DIR}/compile_commands.json"
        SYMBOLIC
    )
endif()

# ============================================================
# Summary
# ============================================================
message(STATUS "MPMCQueue: compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "MPMCQueue: build=${CMAKE_BUILD_TYPE} native=${NATIVE_ARCH} "
               "tests=${BUILD_TESTING_MPMC} bench=${BUILD_BENCHMARK} "
               "asan=${SANITIZE_ADDR} tsan=${SANITIZE_THREAD}")
