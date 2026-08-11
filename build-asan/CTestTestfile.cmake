# CMake generated Testfile for 
# Source directory: /home/matti/Projects/mpmc_queues
# Build directory: /home/matti/Projects/mpmc_queues/build-asan
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[RegistryConformanceTest]=] "/home/matti/Projects/mpmc_queues/build-asan/RegistryConformanceTest")
set_tests_properties([=[RegistryConformanceTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
add_test([=[SegmentLifecycleTest]=] "/home/matti/Projects/mpmc_queues/build-asan/SegmentLifecycleTest")
set_tests_properties([=[SegmentLifecycleTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
add_test([=[TaggingTest]=] "/home/matti/Projects/mpmc_queues/build-asan/TaggingTest")
set_tests_properties([=[TaggingTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
add_test([=[AdmissionTest]=] "/home/matti/Projects/mpmc_queues/build-asan/AdmissionTest")
set_tests_properties([=[AdmissionTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
add_test([=[MemoryLayoutTest]=] "/home/matti/Projects/mpmc_queues/build-asan/MemoryLayoutTest")
set_tests_properties([=[MemoryLayoutTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
add_test([=[PoolReclamationTest]=] "/home/matti/Projects/mpmc_queues/build-asan/PoolReclamationTest")
set_tests_properties([=[PoolReclamationTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
add_test([=[ConcurrencyTest]=] "/home/matti/Projects/mpmc_queues/build-asan/ConcurrencyTest")
set_tests_properties([=[ConcurrencyTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
add_test([=[ThreadRegistryTest]=] "/home/matti/Projects/mpmc_queues/build-asan/ThreadRegistryTest")
set_tests_properties([=[ThreadRegistryTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
add_test([=[ThreadPinnerTest]=] "/home/matti/Projects/mpmc_queues/build-asan/ThreadPinnerTest")
set_tests_properties([=[ThreadPinnerTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
add_test([=[ProxyAccountingTest]=] "/home/matti/Projects/mpmc_queues/build-asan/ProxyAccountingTest")
set_tests_properties([=[ProxyAccountingTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
add_test([=[BucketTest]=] "/home/matti/Projects/mpmc_queues/build-asan/BucketTest")
set_tests_properties([=[BucketTest]=] PROPERTIES  TIMEOUT "900" _BACKTRACE_TRIPLES "/home/matti/Projects/mpmc_queues/CMakeLists.txt;197;add_test;/home/matti/Projects/mpmc_queues/CMakeLists.txt;0;")
subdirs("cmake/extern/googletest")
