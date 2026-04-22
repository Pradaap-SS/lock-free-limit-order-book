cmake_minimum_required(VERSION 3.20)
project(LockFreeOrderBook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Detect architecture
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(IS_ARM TRUE)
    message(STATUS "Building for Apple Silicon (ARM64)")
else()
    set(IS_ARM FALSE)
    message(STATUS "Building for x86_64")
endif()

# Release flags: -O3, native tuning, no exceptions on hot path
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG -fno-exceptions")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -march=native -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# On macOS the CommandLineTools C++/v1 stub is nearly empty; the real libc++
# headers live inside the SDK. Add them explicitly when the stub is incomplete.
if(APPLE)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE MACOS_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(LIBCXX_INCLUDE "${MACOS_SDK_PATH}/usr/include/c++/v1")
    if(EXISTS "${LIBCXX_INCLUDE}/cstdio")
        include_directories(SYSTEM "${LIBCXX_INCLUDE}")
        message(STATUS "Using SDK libc++ headers: ${LIBCXX_INCLUDE}")
    endif()
endif()

# Common include
include_directories(include)

# ── Main library (header-only, but compile a thin object for tests) ──────────
add_library(orderbook_lib INTERFACE)
target_include_directories(orderbook_lib INTERFACE include)

# ── Main executable ──────────────────────────────────────────────────────────
add_executable(order_book src/main.cpp)
target_link_libraries(order_book PRIVATE orderbook_lib)

# ── Google Benchmark ─────────────────────────────────────────────────────────
find_package(benchmark QUIET)
if(benchmark_FOUND)
    add_executable(latency_bench benchmarks/latency_bench.cpp)
    target_link_libraries(latency_bench PRIVATE orderbook_lib benchmark::benchmark)
    message(STATUS "Google Benchmark found — latency_bench target enabled")
else()
    message(WARNING "Google Benchmark not found. Run: brew install google-benchmark")
endif()

# ── Google Test ──────────────────────────────────────────────────────────────
find_package(GTest QUIET)
if(GTest_FOUND)
    enable_testing()
    add_executable(order_book_tests tests/order_book_test.cpp)
    target_link_libraries(order_book_tests PRIVATE orderbook_lib GTest::GTest GTest::Main)
    add_test(NAME OrderBookTests COMMAND order_book_tests)
    message(STATUS "Google Test found — test target enabled")
else()
    message(WARNING "Google Test not found. Run: brew install googletest")
endif()
