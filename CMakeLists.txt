cmake_minimum_required(VERSION 3.20)
project(LockFreeOrderBook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Architecture detection ────────────────────────────────────────────────────
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(IS_ARM TRUE)
    message(STATUS "Building for Apple Silicon (ARM64)")
else()
    set(IS_ARM FALSE)
    message(STATUS "Building for x86_64")
endif()

# ── Compile flags ─────────────────────────────────────────────────────────────
# Release: full optimization + native ISA tuning
set(CMAKE_CXX_FLAGS_RELEASE
    "-O3 -march=native -DNDEBUG -fno-exceptions -fvectorize -fslp-vectorize")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO
    "-O2 -march=native -g -DNDEBUG")
# Debug: address + undefined sanitizers for test builds
set(CMAKE_CXX_FLAGS_DEBUG
    "-O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Per-stage latency tracing (opt-in) ───────────────────────────────────────
option(ENABLE_TRACING "Enable per-stage LatencyTracer instrumentation" OFF)
if(ENABLE_TRACING)
    add_compile_definitions(ENABLE_TRACING)
    message(STATUS "Per-stage latency tracing: ENABLED")
else()
    message(STATUS "Per-stage latency tracing: disabled  (cmake -DENABLE_TRACING=ON to enable)")
endif()

# ── SDK headers fix (macOS CLT stub is incomplete) ──────────────────────────
if(APPLE)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE MACOS_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(LIBCXX_INCLUDE "${MACOS_SDK_PATH}/usr/include/c++/v1")
    if(EXISTS "${LIBCXX_INCLUDE}/cstdio")
        include_directories(SYSTEM "${LIBCXX_INCLUDE}")
        message(STATUS "Using SDK libc++ headers: ${LIBCXX_INCLUDE}")
    endif()
endif()

# ── Common include ────────────────────────────────────────────────────────────
include_directories(include)

# ── Main library (interface target for shared flags) ──────────────────────────
add_library(orderbook_lib INTERFACE)
target_include_directories(orderbook_lib INTERFACE include)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(order_book src/main.cpp)
target_link_libraries(order_book PRIVATE orderbook_lib)

# ── Google Benchmark ──────────────────────────────────────────────────────────
find_package(benchmark QUIET)
if(benchmark_FOUND)
    add_executable(latency_bench benchmarks/latency_bench.cpp)
    target_link_libraries(latency_bench PRIVATE orderbook_lib benchmark::benchmark)
    message(STATUS "Google Benchmark found — latency_bench target enabled")
else()
    message(WARNING "Google Benchmark not found.  Run: brew install google-benchmark")
endif()

# ── Google Test ───────────────────────────────────────────────────────────────
find_package(GTest QUIET)
if(GTest_FOUND)
    enable_testing()
    add_executable(order_book_tests tests/order_book_test.cpp)
    target_link_libraries(order_book_tests PRIVATE orderbook_lib GTest::GTest GTest::Main)
    add_test(NAME OrderBookTests COMMAND order_book_tests)
    message(STATUS "Google Test found — order_book_tests target enabled")
else()
    message(WARNING "Google Test not found.  Run: brew install googletest")
endif()

# ── Tracing build (separate target, no interference with clean Release) ───────
if(benchmark_FOUND)
    add_executable(latency_bench_traced benchmarks/latency_bench.cpp)
    target_compile_definitions(latency_bench_traced PRIVATE ENABLE_TRACING)
    target_link_libraries(latency_bench_traced PRIVATE orderbook_lib benchmark::benchmark)
endif()
