#include <benchmark/benchmark.h>
#include <random>
#include "order_book.h"

// ── Day 1: baseline benchmarks (unchanged) ────────────────────────────────────

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

static void BM_AddOrderAggressive(benchmark::State& state) {
    OrderBook book;
    constexpr int POOL = 2'000'000;
    for (OrderId i = 1; i <= POOL; ++i)
        book.add_order(i, 50'000, 1, Side::Sell);
    OrderId id = POOL + 1;
    for (auto _ : state)
        book.add_order(id++, 50'001, 1, Side::Buy);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AddOrderAggressive)->Iterations(1'000'000)->ReportAggregatesOnly(true);

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

static void BM_TopOfBook(benchmark::State& state) {
    OrderBook book;
    book.add_order(1, 49'999, 1000, Side::Buy);
    book.add_order(2, 50'001, 1000, Side::Sell);
    for (auto _ : state)
        benchmark::DoNotOptimize(book.top_of_book());
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TopOfBook)->Iterations(5'000'000)->ReportAggregatesOnly(true);

// ── Day 2: modify_order benchmarks ───────────────────────────────────────────

// In-place quantity reduction — O(1), preserves time priority
static void BM_ModifyOrder_SamePrice(benchmark::State& state) {
    OrderBook book;
    constexpr int POOL = 1'000'000;
    for (OrderId i = 1; i <= POOL; ++i)
        book.add_order(i, 49'900, 1000, Side::Buy);

    OrderId id = 1;
    Quantity qty = 999;
    for (auto _ : state) {
        if (id > POOL) { id = 1; qty = 999; }
        // Alternate between reducing and restoring (via re-add path)
        // to keep orders alive. For pure in-place timing, keep decreasing.
        book.modify_order(id++, 49'900, qty--);
        if (qty == 0) qty = 999;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ModifyOrder_SamePrice)->Iterations(500'000)->ReportAggregatesOnly(true);

// Price amendment — cancel + re-add path
static void BM_ModifyOrder_DifferentPrice(benchmark::State& state) {
    OrderBook book;
    constexpr int POOL = 1'000'000;
    for (OrderId i = 1; i <= POOL; ++i)
        book.add_order(i, 49'900, 100, Side::Buy);

    OrderId id = 1;
    Price   new_price = 49'800;
    for (auto _ : state) {
        if (id > POOL) { state.SkipWithError("pool exhausted"); break; }
        book.modify_order(id++, new_price--, 100);
        if (new_price < 40'000) new_price = 49'800;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ModifyOrder_DifferentPrice)->Iterations(300'000)->ReportAggregatesOnly(true);

// ── Day 2: get_stats and get_depth ────────────────────────────────────────────

static void BM_GetStats(benchmark::State& state) {
    OrderBook book;
    book.add_order(1, 49'999, 1000, Side::Buy);
    book.add_order(2, 50'001, 1000, Side::Sell);
    for (auto _ : state)
        benchmark::DoNotOptimize(book.get_stats());
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GetStats)->Iterations(5'000'000)->ReportAggregatesOnly(true);

static void BM_GetDepth5(benchmark::State& state) {
    OrderBook book;
    for (int i = 0; i < 10; ++i) {
        book.add_order(100 + i, static_cast<Price>(49'990 - i), 100, Side::Buy);
        book.add_order(200 + i, static_cast<Price>(50'010 + i), 100, Side::Sell);
    }
    std::vector<OrderBook::DepthLevel> bids, asks;
    for (auto _ : state) {
        book.get_depth(bids, asks, 5);
        benchmark::DoNotOptimize(bids.data());
        benchmark::DoNotOptimize(asks.data());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GetDepth5)->Iterations(2'000'000)->ReportAggregatesOnly(true);

// ── Day 2: mixed realistic workload (steady-state) ───────────────────────────
// Simulates a market-maker steadily rolling its quote ladder.
// 50 % add new order, 40 % cancel old order, 10 % in-place modify.
// Net add rate = 10 %, so the 1M pool never exhausts over 2M iterations:
//   seed 50K + 0.1 * 2M net = 250K resting orders at end — well within pool.
static void BM_MixedWorkload(benchmark::State& state) {
    OrderBook book;

    // Seed a realistic-size resting order window
    constexpr OrderId SEED = 50'000;
    for (OrderId i = 1; i <= SEED; ++i) {
        bool buy = (i & 1) == 0;
        book.add_order(i, buy ? 49'900u : 50'100u, 100,
                       buy ? Side::Buy : Side::Sell);
    }

    OrderId add_id    = SEED + 1;
    OrderId cancel_id = 1;
    uint64_t n = 0;

    for (auto _ : state) {
        const uint8_t r = static_cast<uint8_t>(n++ % 10);

        if (r < 5) {
            // 50 %: add a new passive order
            bool buy = (add_id & 1) == 0;
            book.add_order(add_id++, buy ? 49'900u : 50'100u, 100,
                           buy ? Side::Buy : Side::Sell);
        } else if (r < 9) {
            // 40 %: cancel the oldest resting order
            if (cancel_id < add_id) book.cancel_order(cancel_id++);
        } else {
            // 10 %: in-place modify (qty reduction on a mid-range order)
            const OrderId mid = cancel_id + (add_id - cancel_id) / 2;
            if (mid > cancel_id) book.modify_order(mid, 49'900u, 80);
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MixedWorkload)->Iterations(2'000'000)->ReportAggregatesOnly(true);

BENCHMARK_MAIN();
