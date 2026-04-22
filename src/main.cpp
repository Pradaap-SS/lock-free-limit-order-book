#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

#include "order_book.h"
#include "timing.h"

// ── Latency histogram ────────────────────────────────────────────────────────
struct LatencyStats {
    std::vector<uint64_t> samples;

    void reserve(std::size_t n) { samples.reserve(n); }
    void record(uint64_t ticks) { samples.push_back(ticks); }

    void print(const char* label) const {
        if (samples.empty()) { std::printf("%-20s  (no samples)\n", label); return; }
        auto s = samples;
        std::sort(s.begin(), s.end());

        auto ns_at = [&](double pct) -> double {
            std::size_t i = static_cast<std::size_t>(pct / 100.0 * (s.size() - 1));
            return ticks_to_ns(s[i]);
        };

        std::printf("%-20s  p50=%6.0fns  p95=%6.0fns  p99=%6.0fns  p99.9=%7.0fns  p99.99=%8.0fns  n=%zu\n",
            label,
            ns_at(50), ns_at(95), ns_at(99), ns_at(99.9), ns_at(99.99),
            s.size());
    }
};

int main() {
    std::printf("=== Lock-Free Limit Order Book — Smoke Test & Latency Sample ===\n\n");

#if defined(__APPLE__)
    std::printf("Timer: mach_absolute_time  (~%llu MHz)\n\n",
        (unsigned long long)approx_tick_hz() / 1'000'000);
#else
    std::printf("Calibrating TSC frequency...\n");
    uint64_t freq_hz = calibrate_tsc_hz();
    std::printf("TSC frequency: %.3f GHz\n\n", freq_hz / 1e9);
#endif

    // ── Functional smoke test ─────────────────────────────────────────────────
    {
        int trade_count = 0;
        OrderBook book([&](const Event& e) {
            if (e.type == EventType::Trade) {
                std::printf("  TRADE: aggressor=%llu passive=%llu price=%u qty=%u\n",
                    (unsigned long long)e.trade.aggressor_id,
                    (unsigned long long)e.trade.passive_id,
                    e.trade.price, e.trade.quantity);
                ++trade_count;
            }
        });

        std::printf("--- Resting orders ---\n");
        book.add_order(1, 100, 500, Side::Buy);
        book.add_order(2,  99, 300, Side::Buy);
        book.add_order(3, 101, 200, Side::Sell);
        book.add_order(4, 102, 400, Side::Sell);

        auto tob = book.top_of_book();
        std::printf("Top of book: bid=%u@%u  ask=%u@%u\n\n",
            tob.bid_qty, tob.bid_price, tob.ask_qty, tob.ask_price);

        std::printf("--- Aggressive buy crosses ask ---\n");
        book.add_order(5, 103, 150, Side::Buy);

        tob = book.top_of_book();
        std::printf("After match: bid=%u@%u  ask=%u@%u\n\n",
            tob.bid_qty, tob.bid_price, tob.ask_qty, tob.ask_price);

        std::printf("--- Cancel resting bid ---\n");
        book.cancel_order(1);
        tob = book.top_of_book();
        std::printf("After cancel: bid=%u@%u  ask=%u@%u\n\n",
            tob.bid_qty, tob.bid_price, tob.ask_qty, tob.ask_price);

        std::printf("Trades executed: %d (expected 1)\n\n", trade_count);
    }

    // ── Latency sampling ──────────────────────────────────────────────────────
    std::printf("--- Latency sampling (1,000,000 add_order + 250,000 cancel_order) ---\n");
    {
        OrderBook book;
        constexpr int N = 1'000'000;
        LatencyStats add_stats, cancel_stats;
        add_stats.reserve(N);
        cancel_stats.reserve(N / 4);

        uint64_t t0, t1;

        for (int i = 1; i <= N; ++i) {
            // Non-crossing prices: buys below 49000, sells above 51000
            Price price = (i % 2 == 0)
                ? static_cast<Price>(48'000 + (i % 1000))
                : static_cast<Price>(52'000 + (i % 1000));
            Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;

            t0 = read_cycles();
            book.add_order(static_cast<OrderId>(i), price, 100, side);
            t1 = read_cycles();
            add_stats.record(t1 - t0);

            // Cancel ~25% — always a valid resting order
            if (i % 4 == 0) {
                OrderId cid = static_cast<OrderId>(i - 2); // 2 behind: guaranteed resting
                t0 = read_cycles();
                book.cancel_order(cid);
                t1 = read_cycles();
                cancel_stats.record(t1 - t0);
            }
        }

        add_stats.print("add_order");
        cancel_stats.print("cancel_order");
    }

    std::printf("\nDone.\n");
    return 0;
}
