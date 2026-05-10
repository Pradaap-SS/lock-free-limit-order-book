#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <thread>

#include "order_book.h"
#include "timing.h"
#include "cpu_affinity.h"

// ── Latency histogram (manual sampling) ──────────────────────────────────────
struct LatencyStats {
    std::vector<uint64_t> samples;
    void reserve(std::size_t n) { samples.reserve(n); }
    void record(uint64_t ticks) { samples.push_back(ticks); }

    void print(const char* label) const {
        if (samples.empty()) { std::printf("%-22s  (no samples)\n", label); return; }
        auto s = samples;
        std::sort(s.begin(), s.end());
        auto ns_at = [&](double pct) {
            return ticks_to_ns(s[static_cast<std::size_t>(pct / 100.0 * (s.size() - 1))]);
        };
        std::printf("%-22s  p50=%5.0fns  p95=%5.0fns  p99=%6.0fns  p99.9=%7.0fns  n=%zu\n",
            label, ns_at(50), ns_at(95), ns_at(99), ns_at(99.9), s.size());
    }
};

// ── Helpers ──────────────────────────────────────────────────────────────────
static void print_tob(const OrderBook& book) {
    auto tob = book.top_of_book();
    std::printf("  ToB: bid=%u@%u  ask=%u@%u\n",
        tob.bid_qty, tob.bid_price, tob.ask_qty, tob.ask_price);
}

static void print_stats(const OrderBook& book) {
    auto s = book.get_stats();
    std::printf("  Stats: spread=%u  mid=%u  bidQty=%llu  askQty=%llu  orders=%zu\n",
        s.spread, s.mid,
        (unsigned long long)s.bid_total_qty,
        (unsigned long long)s.ask_total_qty,
        s.orders_in_flight);
}

static void print_depth(const OrderBook& book, std::size_t n = 3) {
    std::vector<OrderBook::DepthLevel> bids, asks;
    book.get_depth(bids, asks, n);
    std::printf("  Depth (top %zu):\n", n);
    for (auto& l : asks) {
        std::printf("    ASK  price=%-5u  qty=%-6u  orders=%u\n",
            l.price, l.total_qty, l.order_count);
    }
    std::printf("    ----  SPREAD  ----\n");
    for (auto& l : bids) {
        std::printf("    BID  price=%-5u  qty=%-6u  orders=%u\n",
            l.price, l.total_qty, l.order_count);
    }
}

// ── Section 1: CPU affinity setup ────────────────────────────────────────────
static void demo_affinity() {
    std::printf("=== CPU Affinity ===\n");
    std::printf("  Logical cores: %d\n", logical_core_count());
    std::printf("  Performance cores: %d\n", performance_core_count());

    // Pin to core 0 (first P-core on Apple Silicon)
    bool pinned = pin_thread_to_core(0);
    std::printf("  Pin main thread to core 0: %s\n", pinned ? "OK" : "FAILED (need sudo on some systems)");

    bool rt = set_realtime_priority();
    std::printf("  Set realtime priority: %s\n\n", rt ? "OK" : "skipped (need privileges)");
}

// ── Section 2: Functional smoke test (Day 1 recap) ───────────────────────────
static void demo_smoke_test() {
    std::printf("=== Smoke Test ===\n");
    int trades = 0;
    OrderBook book([&](const Event& e) {
        if (e.type == EventType::Trade)
            std::printf("  TRADE: aggressor=%llu passive=%llu price=%u qty=%u\n",
                (unsigned long long)e.trade.aggressor_id,
                (unsigned long long)e.trade.passive_id,
                e.trade.price, e.trade.quantity);
        if (e.type == EventType::Trade) ++trades;
    });

    book.add_order(1, 100, 500, Side::Buy);
    book.add_order(2,  99, 300, Side::Buy);
    book.add_order(3, 101, 200, Side::Sell);
    book.add_order(4, 102, 400, Side::Sell);
    print_tob(book);

    // Aggressive buy: crosses asks
    book.add_order(5, 103, 150, Side::Buy);
    print_tob(book);
    std::printf("  Trades: %d (expected 1)\n\n", trades);
}

// ── Section 3: modify_order demo ─────────────────────────────────────────────
static void demo_modify() {
    std::printf("=== modify_order ===\n");
    OrderBook book([](const Event& e) {
        if (e.type == EventType::OrderModify)
            std::printf("  MODIFY (in-place): id=%llu\n",
                (unsigned long long)e.order_id);
        else if (e.type == EventType::OrderCancel)
            std::printf("  CANCEL (re-add path): id=%llu\n",
                (unsigned long long)e.order_id);
        else if (e.type == EventType::OrderAck)
            std::printf("  ACK: id=%llu\n",
                (unsigned long long)e.order_id);
    });

    // Seed the book
    book.add_order(10, 50'000, 1000, Side::Buy);
    book.add_order(11, 50'001,  500, Side::Sell);
    std::printf("Before modify:\n");
    print_tob(book);
    print_stats(book);

    // Case 1: same price, quantity reduction — in-place O(1), no priority loss
    std::printf("\nmodify_order(10, price=50000, qty=400) — in-place reduction:\n");
    book.modify_order(10, 50'000, 400);
    print_stats(book);

    // Case 2: price change — cancel + re-add, loses time priority
    std::printf("\nmodify_order(10, price=49999, qty=400) — price amendment:\n");
    book.modify_order(10, 49'999, 400);
    print_tob(book);
    std::printf("\n");
}

// ── Section 4: Multi-level matching demo ─────────────────────────────────────
static void demo_multilevel() {
    std::printf("=== Multi-Level Matching ===\n");
    int trades = 0;
    OrderBook book([&](const Event& e) {
        if (e.type == EventType::Trade) {
            std::printf("  TRADE price=%u qty=%u\n",
                e.trade.price, e.trade.quantity);
            ++trades;
        }
    });

    // Build an ask side with three price levels
    book.add_order(1, 100, 200, Side::Sell);
    book.add_order(2, 101, 300, Side::Sell);
    book.add_order(3, 102, 400, Side::Sell);
    std::printf("Ask depth before aggressive buy:\n");
    print_depth(book, 3);

    // One large aggressive buy sweeps through all three levels
    std::printf("\nAggressive buy: price=103, qty=900\n");
    book.add_order(4, 103, 900, Side::Buy);
    print_tob(book);
    std::printf("  Trades executed: %d (expected 3)\n\n", trades);
}

// ── Section 5: Book stats & depth snapshot ───────────────────────────────────
static void demo_depth() {
    std::printf("=== Depth Snapshot ===\n");
    OrderBook book;

    // Build a realistic-looking 5-level book
    for (int i = 0; i < 5; ++i) {
        book.add_order(100 + i, static_cast<Price>(50'000 - i),
                       static_cast<Quantity>(100 * (i + 1)), Side::Buy);
        book.add_order(200 + i, static_cast<Price>(50'001 + i),
                       static_cast<Quantity>(80 * (i + 1)),  Side::Sell);
    }
    print_depth(book, 5);
    print_stats(book);
    std::printf("\n");
}

// ── Section 6: Latency sampling (1M ops) ─────────────────────────────────────
static void demo_latency() {
    std::printf("=== Latency Sampling (1,000,000 iterations) ===\n");

    // Pin to core 0 for tighter measurements
    pin_thread_to_core(0);

    OrderBook book;
    constexpr int N = 1'000'000;
    LatencyStats add_stats, cancel_stats, modify_stats;
    add_stats.reserve(N);
    cancel_stats.reserve(N / 4);
    modify_stats.reserve(N / 4);

    uint64_t t0, t1;
    for (int i = 1; i <= N; ++i) {
        // Non-crossing prices
        bool is_buy = (i % 2 == 0);
        Price price = is_buy ? static_cast<Price>(48'000 + (i % 1000))
                             : static_cast<Price>(52'000 + (i % 1000));
        Side side = is_buy ? Side::Buy : Side::Sell;

        t0 = read_cycles();
        book.add_order(static_cast<OrderId>(i), price, 100, side);
        t1 = read_cycles();
        add_stats.record(t1 - t0);

        // Every 4th: cancel a resting order
        if (i % 4 == 0) {
            OrderId cid = static_cast<OrderId>(i - 2);
            t0 = read_cycles();
            book.cancel_order(cid);
            t1 = read_cycles();
            cancel_stats.record(t1 - t0);
        }

        // Every 8th: in-place modify (same price, reduced qty)
        if (i % 8 == 0) {
            OrderId mid_id = static_cast<OrderId>(i - 4);
            // Only attempt if order might still be resting (not cancelled)
            t0 = read_cycles();
            book.modify_order(mid_id, price, 50); // reduce to 50
            t1 = read_cycles();
            modify_stats.record(t1 - t0);
        }
    }

    add_stats.print("add_order");
    cancel_stats.print("cancel_order");
    modify_stats.print("modify_order (in-place)");
    std::printf("\n");
}

// ── Section 7: Per-stage tracer ───────────────────────────────────────────────
static void demo_tracer() {
    std::printf("=== Per-Stage Latency Tracer ===\n");

    OrderBook book;
    constexpr int N = 200'000;

    for (int i = 1; i <= N; ++i) {
        bool is_buy = (i % 2 == 0);
        Price price = is_buy ? static_cast<Price>(48'000 + (i % 500))
                             : static_cast<Price>(52'000 + (i % 500));
        book.add_order(static_cast<OrderId>(i), price, 100,
                       is_buy ? Side::Buy : Side::Sell);
        if (i % 5 == 0)
            book.cancel_order(static_cast<OrderId>(i - 2));
        if (i % 7 == 0)
            book.modify_order(static_cast<OrderId>(i - 3), price, 50);
    }

    book.tracer().print();

    // Optionally dump CSV for Python plotting
    book.tracer().dump_csv("docs/benchmarks/stages.csv");
    std::printf("\n");
}

// ── main ─────────────────────────────────────────────────────────────────────
int main() {
    std::printf("╔══════════════════════════════════════════════════════╗\n");
    std::printf("║   Lock-Free Order Book — Day 2 Demo                 ║\n");
    std::printf("╚══════════════════════════════════════════════════════╝\n\n");

#if defined(__APPLE__)
    std::printf("Timer: mach_absolute_time (~%llu MHz)\n\n",
        (unsigned long long)approx_tick_hz() / 1'000'000);
#else
    auto freq = calibrate_tsc_hz();
    std::printf("TSC: %.3f GHz\n\n", freq / 1e9);
#endif

    demo_affinity();
    demo_smoke_test();
    demo_modify();
    demo_multilevel();
    demo_depth();
    demo_latency();
    demo_tracer();

    std::printf("Done.\n");
    return 0;
}
