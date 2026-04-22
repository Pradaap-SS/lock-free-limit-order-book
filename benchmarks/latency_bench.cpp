#include <benchmark/benchmark.h>
#include "order_book.h"

// ── Passive add (no match) ────────────────────────────────────────────────────
// Measures pure insertion latency. Prices never cross.
static void BM_AddOrderPassive(benchmark::State& state) {
    OrderBook book;
    OrderId id = 1;
    for (auto _ : state) {
        bool is_buy = (id % 2 == 0);
        Price price = is_buy ? 49'900u : 50'100u;
        book.add_order(id++, price, 100, is_buy ? Side::Buy : Side::Sell);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AddOrderPassive)->Iterations(2'000'000)->ReportAggregatesOnly(true);

// ── Aggressive add (matches) ──────────────────────────────────────────────────
// Measures matching latency. We refill the book with passive orders in Setup
// and then fire aggressors that match 1-for-1.
static void BM_AddOrderAggressive(benchmark::State& state) {
    // Build a pool of resting sells that will be consumed one at a time
    OrderBook book;
    constexpr int POOL = 2'000'000;
    for (OrderId i = 1; i <= POOL; ++i)
        book.add_order(i, 50'000, 1, Side::Sell); // qty=1 so each aggressor fully fills one

    OrderId id = POOL + 1;
    for (auto _ : state) {
        book.add_order(id++, 50'001, 1, Side::Buy); // crosses ask at 50000
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AddOrderAggressive)->Iterations(1'000'000)->ReportAggregatesOnly(true);

// ── Cancel order ──────────────────────────────────────────────────────────────
static void BM_CancelOrder(benchmark::State& state) {
    OrderBook book;
    constexpr int POOL = 2'000'000;
    for (OrderId i = 1; i <= POOL; ++i)
        book.add_order(i, 49'900, 100, Side::Buy);

    OrderId id = 1;
    for (auto _ : state) {
        if (id > POOL) { state.SkipWithError("pool exhausted"); break; }
        book.cancel_order(id++);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CancelOrder)->Iterations(1'000'000)->ReportAggregatesOnly(true);

// ── Top-of-book query ─────────────────────────────────────────────────────────
static void BM_TopOfBook(benchmark::State& state) {
    OrderBook book;
    book.add_order(1, 49'999, 1000, Side::Buy);
    book.add_order(2, 50'001, 1000, Side::Sell);
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.top_of_book());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TopOfBook)->Iterations(5'000'000)->ReportAggregatesOnly(true);

BENCHMARK_MAIN();
