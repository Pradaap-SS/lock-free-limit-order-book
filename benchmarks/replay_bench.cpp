#include <benchmark/benchmark.h>
#include <vector>
#include <cstdint>
#include "replay_engine.h"
#include "itch_parser.h"

// ── Synthetic stream fixture ──────────────────────────────────────────────────
// Built once and reused across benchmark iterations to avoid measuring
// stream construction overhead.

struct ItchFixture {
    std::vector<uint8_t> buf;

    // Generate N messages: alternating Add Buy / Add Sell, then Delete oldest
    explicit ItchFixture(int n_messages, const char* sym = "AAPL",
                         uint16_t locate = 1) {
        buf.reserve(n_messages * 40);
        uint8_t tmp[64];

        // Stock directory
        buf.insert(buf.end(), tmp,
            tmp + itch_build::stock_directory(tmp, locate, 0, sym));

        uint64_t ref = 1;
        uint64_t ts  = 1'000'000;
        const uint32_t BASE_PRICE = 1'500'000;

        for (int i = 0; i < n_messages; ++i) {
            ts += 500;
            int r = i % 10;

            if (r < 4) {
                // 40 % add bid
                uint32_t price = BASE_PRICE - static_cast<uint32_t>((i % 20) * 100);
                buf.insert(buf.end(), tmp,
                    tmp + itch_build::add_order(tmp, locate, ts, ref++,
                                                'B', 100, sym, price));
            } else if (r < 8) {
                // 40 % add ask
                uint32_t price = BASE_PRICE + static_cast<uint32_t>((i % 20 + 1) * 100);
                buf.insert(buf.end(), tmp,
                    tmp + itch_build::add_order(tmp, locate, ts, ref++,
                                                'S', 100, sym, price));
            } else if (r < 9 && ref > 30) {
                // 10 % delete (oldest resting order)
                buf.insert(buf.end(), tmp,
                    tmp + itch_build::order_delete(tmp, locate, ts, ref - 30));
            } else {
                // 10 % partial execution
                if (ref > 20) {
                    buf.insert(buf.end(), tmp,
                        tmp + itch_build::order_executed(tmp, locate, ts,
                                                         ref - 20, 50, ref));
                }
            }
        }
    }
};

// Build fixtures once at process start (shared across benchmarks)
static const ItchFixture kFix100K{100'000};
static const ItchFixture kFix1M{1'000'000};

// ── Benchmark: raw ITCH parse throughput (no order book) ─────────────────────
// Measures how fast we can walk the binary buffer and dispatch callbacks.
static void BM_ItchParseOnly(benchmark::State& state) {
    const auto& fix = kFix100K;
    uint64_t total_msgs = 0;

    for (auto _ : state) {
        ItchParser p;
        uint64_t count = 0;
        p.on_add_order       = [&](auto&) { ++count; };
        p.on_order_delete    = [&](auto&) { ++count; };
        p.on_order_executed  = [&](auto&) { ++count; };
        p.on_stock_directory = [&](auto&) { ++count; };
        total_msgs = p.parse(fix.buf.data(), fix.buf.size());
        benchmark::DoNotOptimize(count);
    }
    state.SetBytesProcessed(state.iterations() *
                            static_cast<int64_t>(fix.buf.size()));
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(total_msgs));
}
BENCHMARK(BM_ItchParseOnly)->Iterations(200)->ReportAggregatesOnly(true);

// ── Benchmark: full replay (parse + order book ops) ──────────────────────────
static void BM_ReplayFull100K(benchmark::State& state) {
    const auto& fix = kFix100K;

    for (auto _ : state) {
        ReplayEngine engine;
        ReplayEngine::Config cfg;
        cfg.symbol = "AAPL";
        cfg.tick_divisor = 100;
        auto snap = engine.run(fix.buf.data(), fix.buf.size(), cfg);
        benchmark::DoNotOptimize(snap.orders_added);
    }
    state.SetItemsProcessed(state.iterations() * 100'000LL);
}
BENCHMARK(BM_ReplayFull100K)->Iterations(100)->ReportAggregatesOnly(true);

// ── Benchmark: 1M message replay ─────────────────────────────────────────────
static void BM_ReplayFull1M(benchmark::State& state) {
    const auto& fix = kFix1M;

    for (auto _ : state) {
        ReplayEngine engine;
        ReplayEngine::Config cfg;
        cfg.symbol = "AAPL";
        cfg.tick_divisor = 100;
        auto snap = engine.run(fix.buf.data(), fix.buf.size(), cfg);
        benchmark::DoNotOptimize(snap.orders_added);
    }
    state.SetItemsProcessed(state.iterations() * 1'000'000LL);
}
BENCHMARK(BM_ReplayFull1M)->Iterations(10)->ReportAggregatesOnly(true);

// ── Benchmark: parser with symbol filter active ───────────────────────────────
// Same stream but filtered to a symbol that doesn't exist — measures
// the overhead of locate-based filtering.
static void BM_ReplaySymbolFilter(benchmark::State& state) {
    const auto& fix = kFix100K;

    for (auto _ : state) {
        ReplayEngine engine;
        ReplayEngine::Config cfg;
        cfg.symbol = "ZZZZ"; // no match → all messages filtered at locate check
        cfg.tick_divisor = 100;
        auto snap = engine.run(fix.buf.data(), fix.buf.size(), cfg);
        benchmark::DoNotOptimize(snap.messages_processed);
    }
    state.SetItemsProcessed(state.iterations() * 100'000LL);
}
BENCHMARK(BM_ReplaySymbolFilter)->Iterations(200)->ReportAggregatesOnly(true);

// ── Benchmark: big-endian field decoding ─────────────────────────────────────
// Baseline: just parsing all AddOrder fields with no order book involvement.
static void BM_AddOrderDecodeOnly(benchmark::State& state) {
    // Build a stream of pure AddOrder messages
    ItchFixture add_only{0}; // empty, we'll build manually
    uint8_t tmp[64];
    add_only.buf.insert(add_only.buf.end(), tmp,
        tmp + itch_build::stock_directory(tmp, 1, 0, "AAPL"));
    for (int i = 1; i <= 100'000; ++i) {
        add_only.buf.insert(add_only.buf.end(), tmp,
            tmp + itch_build::add_order(tmp, 1, i * 1000,
                                        static_cast<uint64_t>(i),
                                        (i % 2 == 0) ? 'B' : 'S',
                                        100, "AAPL",
                                        1'500'000 + i * 10));
    }

    uint64_t total_price = 0;
    for (auto _ : state) {
        ItchParser p;
        p.on_add_order = [&](const AddOrderMsg& m) {
            total_price += m.price; // prevent optimizing away
        };
        p.parse(add_only.buf.data(), add_only.buf.size());
        benchmark::DoNotOptimize(total_price);
    }
    state.SetItemsProcessed(state.iterations() * 100'000LL);
}
BENCHMARK(BM_AddOrderDecodeOnly)->Iterations(200)->ReportAggregatesOnly(true);

BENCHMARK_MAIN();
