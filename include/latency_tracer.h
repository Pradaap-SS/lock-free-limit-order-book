#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include "timing.h"

// Per-stage latency breakdown for the order book hot path.
//
// Build with tracing: cmake ... -DENABLE_TRACING=ON
// Without that flag every method is a no-op and compiles to nothing.
//
// Stages measured:
//   PoolAcquire   — pool_->acquire()
//   MapInsert     — order_map_->insert()
//   CrossCheck    — price-crossing detection branch
//   MatchExecute  — full match() body
//   LevelInsert   — price_level.push_back()
//   CancelLookup  — order_map_->find() + level.remove() + pool_->release()
//   ModifyInPlace — in-place quantity reduction path

enum class Stage : uint8_t {
    PoolAcquire = 0,
    MapInsert,
    CrossCheck,
    MatchExecute,
    LevelInsert,
    CancelLookup,
    ModifyInPlace,
    COUNT
};

static constexpr const char* kStageNames[] = {
    "pool_acquire", "map_insert",   "cross_check",
    "match_execute","level_insert", "cancel_lookup", "modify_inplace"
};

// ── Enabled build ─────────────────────────────────────────────────────────────
#ifdef ENABLE_TRACING

#include <vector>

class LatencyTracer {
    static constexpr std::size_t CAPACITY = 2'000'000;

    struct StageBuf {
        std::unique_ptr<uint64_t[]> data{new uint64_t[CAPACITY]{}};
        std::size_t n{0};

        void record(uint64_t t) noexcept { if (n < CAPACITY) data[n++] = t; }
    };

    std::array<StageBuf, static_cast<std::size_t>(Stage::COUNT)> bufs_;

public:
    void record(Stage s, uint64_t ticks) noexcept {
        bufs_[static_cast<uint8_t>(s)].record(ticks);
    }

    void reset() noexcept { for (auto& b : bufs_) b.n = 0; }

    void print() const {
        std::printf("\n=== Per-Stage Latency Breakdown ===\n");
        std::printf("%-16s  %9s  %7s  %7s  %8s  %9s\n",
                    "Stage", "Samples", "p50", "p99", "p99.9", "p99.99");
        std::printf("%-16s  %9s  %7s  %7s  %8s  %9s\n",
                    "-----", "-------", "---", "---", "-----", "------");
        for (uint8_t i = 0; i < static_cast<uint8_t>(Stage::COUNT); ++i) {
            const auto& b = bufs_[i];
            if (b.n == 0) continue;
            std::vector<uint64_t> s(b.data.get(), b.data.get() + b.n);
            std::sort(s.begin(), s.end());
            auto ns = [&](double pct) {
                return ticks_to_ns(s[static_cast<std::size_t>(pct / 100.0 * (s.size() - 1))]);
            };
            std::printf("%-16s  %9zu  %6.0fns  %6.0fns  %7.0fns  %8.0fns\n",
                kStageNames[i], b.n, ns(50), ns(99), ns(99.9), ns(99.99));
        }
    }

    // Dump raw nanosecond samples to CSV for Python plotting
    void dump_csv(const char* path) const {
        FILE* f = std::fopen(path, "w");
        if (!f) { std::perror(path); return; }
        std::fprintf(f, "stage,ns\n");
        for (uint8_t i = 0; i < static_cast<uint8_t>(Stage::COUNT); ++i) {
            const auto& b = bufs_[i];
            for (std::size_t j = 0; j < b.n; ++j)
                std::fprintf(f, "%s,%.2f\n", kStageNames[i], ticks_to_ns(b.data[j]));
        }
        std::fclose(f);
        std::printf("Stage CSV written to: %s\n", path);
    }
};

// RAII stage timer — records elapsed ticks into the tracer on destruction
struct StageTimer {
    LatencyTracer& tracer;
    Stage          stage;
    uint64_t       start;

    StageTimer(LatencyTracer& t, Stage s) noexcept
        : tracer(t), stage(s), start(read_cycles()) {}
    ~StageTimer() noexcept { tracer.record(stage, read_cycles() - start); }
};

#define STAGE_TIME(tracer, stage) StageTimer _st{tracer, stage}

// ── Disabled build (zero overhead) ───────────────────────────────────────────
#else

class LatencyTracer {
public:
    void record(Stage, uint64_t) noexcept {}
    void reset() noexcept {}
    void print() const {
        std::printf("Tracing disabled. Rebuild with -DENABLE_TRACING=ON\n");
    }
    void dump_csv(const char*) const {}
};

#define STAGE_TIME(tracer, stage) (void)0

#endif
