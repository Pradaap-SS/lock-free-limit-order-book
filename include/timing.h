#pragma once
#include <cstdint>

// High-resolution timestamp for latency measurement.
//
// macOS / Apple Silicon: mach_absolute_time() — userspace call backed by
//   the hardware virtual counter (cntvct_el0) via commpage, no syscall.
//   Convert to ns with mach_timebase_info.
//
// x86_64: RDTSC with LFENCE serialization. Calibrate TSC Hz at startup;
//   pass freq_hz to ticks_to_ns().

#if defined(__APPLE__)
#include <mach/mach_time.h>

inline uint64_t read_cycles() noexcept {
    return mach_absolute_time();
}

// Returns nanoseconds per Mach tick (numerator/denominator ratio).
// Computed once at startup; cheap after that.
struct MachTimebase {
    mach_timebase_info_data_t info{};
    MachTimebase() { mach_timebase_info(&info); }
    double ticks_to_ns(uint64_t ticks) const noexcept {
        return static_cast<double>(ticks)
             * static_cast<double>(info.numer)
             / static_cast<double>(info.denom);
    }
};

// Global singleton — initialised once, then read-only
inline const MachTimebase& timebase() {
    static MachTimebase tb;
    return tb;
}

inline double ticks_to_ns(uint64_t ticks) noexcept {
    return timebase().ticks_to_ns(ticks);
}

// Approximate tick frequency in Hz (for display only)
inline uint64_t approx_tick_hz() noexcept {
    // On Apple Silicon the Mach timebase numerator==numer, denom==denom,
    // and frequency ≈ 1e9 * denom / numer
    auto& tb = timebase().info;
    return static_cast<uint64_t>(1e9 * tb.denom / tb.numer);
}

#else // x86_64 non-Apple

inline uint64_t read_cycles() noexcept {
    uint32_t lo, hi;
    asm volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline double ticks_to_ns(uint64_t ticks, uint64_t freq_hz) noexcept {
    return static_cast<double>(ticks) * 1e9 / static_cast<double>(freq_hz);
}

// Calibrate TSC frequency against steady_clock (call once at startup)
#include <chrono>
inline uint64_t calibrate_tsc_hz() noexcept {
    using Clock = std::chrono::steady_clock;
    const uint64_t t0 = read_cycles();
    const auto     w0 = Clock::now();
    volatile uint64_t spin = 0;
    while (Clock::now() - w0 < std::chrono::milliseconds(200)) ++spin;
    const uint64_t t1 = read_cycles();
    const auto     w1 = Clock::now();
    double secs = std::chrono::duration<double>(w1 - w0).count();
    return static_cast<uint64_t>((t1 - t0) / secs);
}

#endif

// RAII scope timer — records elapsed ticks into *out on destruction
struct ScopeTimer {
    uint64_t  start;
    uint64_t* out;
    explicit ScopeTimer(uint64_t* out) noexcept : start(read_cycles()), out(out) {}
    ~ScopeTimer() noexcept { *out = read_cycles() - start; }
};
