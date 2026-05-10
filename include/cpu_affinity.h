#pragma once
#include <cstdint>
#include <cstdio>

// Thread-to-core affinity helpers.
//
// macOS note: the kernel treats affinity as a scheduling *hint*, not a hard
// bind. The OS will try to keep threads on their preferred core but may move
// them. This still meaningfully reduces context-switch jitter vs. no affinity.
// Linux isolcpus + pthread_setaffinity_np gives stricter guarantees.

#if defined(__APPLE__)
#include <pthread.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <sys/sysctl.h>

// Pin the calling thread to the given logical core (0-indexed).
// Returns true on success.
inline bool pin_thread_to_core(int core_id) noexcept {
    thread_affinity_policy_data_t policy{core_id};
    kern_return_t r = thread_policy_set(
        pthread_mach_thread_np(pthread_self()),
        THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_AFFINITY_POLICY_COUNT);
    return r == KERN_SUCCESS;
}

// Boost the calling thread's QoS to user-interactive (reduces OS preemption).
inline bool set_realtime_priority() noexcept {
    // On macOS, THREAD_TIME_CONSTRAINT_POLICY gives soft real-time guarantees.
    // Values below are conservative — 1 ms period, 0.5 ms computation.
    thread_time_constraint_policy_data_t policy{};
    policy.period      = 1'000'000; // mach ticks (~1 ms on Apple Silicon)
    policy.computation =   500'000;
    policy.constraint  = 1'000'000;
    policy.preemptible = FALSE;
    kern_return_t r = thread_policy_set(
        pthread_mach_thread_np(pthread_self()),
        THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    return r == KERN_SUCCESS;
}

// Number of logical (P+E) cores available.
inline int logical_core_count() noexcept {
    int n = 1;
    std::size_t len = sizeof(n);
    sysctlbyname("hw.logicalcpu", &n, &len, nullptr, 0);
    return n;
}

// Performance core count (Apple Silicon only; falls back to logical_core_count).
inline int performance_core_count() noexcept {
    int n = 0;
    std::size_t len = sizeof(n);
    if (sysctlbyname("hw.perflevel0.logicalcpu", &n, &len, nullptr, 0) == 0)
        return n;
    return logical_core_count();
}

#else // Linux / generic POSIX

#include <pthread.h>
#include <sched.h>

inline bool pin_thread_to_core(int core_id) noexcept {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core_id, &cs);
    return pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs) == 0;
}

inline bool set_realtime_priority() noexcept {
    struct sched_param p{ sched_get_priority_max(SCHED_FIFO) };
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0;
}

inline int logical_core_count() noexcept {
    return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
}

inline int performance_core_count() noexcept {
    return logical_core_count();
}

#endif
