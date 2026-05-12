#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>

#include "replay_engine.h"
#include "mmap_reader.h"
#include "itch_parser.h"
#include "timing.h"

// ── Synthetic ITCH stream builder ─────────────────────────────────────────────
// Generates a realistic mix of ITCH messages for a single symbol.
// Used when no real ITCH file is provided.

struct SyntheticStream {
    std::vector<uint8_t> buf;

    SyntheticStream() { buf.reserve(16 * 1024 * 1024); } // 16 MB initial

    void append(const uint8_t* data, std::size_t n) {
        buf.insert(buf.end(), data, data + n);
    }

    void stock_dir(uint16_t locate, uint64_t ts, const char* sym) {
        uint8_t tmp[64];
        std::size_t n = itch_build::stock_directory(tmp, locate, ts, sym);
        append(tmp, n);
    }

    void add(uint64_t ts, uint64_t ref, char side, uint32_t qty,
             const char* sym, uint32_t price, uint16_t locate = 1) {
        uint8_t tmp[64];
        std::size_t n = itch_build::add_order(tmp, locate, ts, ref, side, qty, sym, price);
        append(tmp, n);
    }

    void exec(uint64_t ts, uint64_t ref, uint32_t exec_qty,
              uint64_t match, uint16_t locate = 1) {
        uint8_t tmp[64];
        std::size_t n = itch_build::order_executed(tmp, locate, ts, ref, exec_qty, match);
        append(tmp, n);
    }

    void cancel(uint64_t ts, uint64_t ref, uint32_t cancel_qty,
                uint16_t locate = 1) {
        uint8_t tmp[64];
        std::size_t n = itch_build::order_cancel(tmp, locate, ts, ref, cancel_qty);
        append(tmp, n);
    }

    void del(uint64_t ts, uint64_t ref, uint16_t locate = 1) {
        uint8_t tmp[64];
        std::size_t n = itch_build::order_delete(tmp, locate, ts, ref);
        append(tmp, n);
    }

    void replace(uint64_t ts, uint64_t orig, uint64_t next_ref,
                 uint32_t qty, uint32_t price, uint16_t locate = 1) {
        uint8_t tmp[64];
        std::size_t n = itch_build::order_replace(tmp, locate, ts, orig, next_ref, qty, price);
        append(tmp, n);
    }
};

// ── Section 1: parser correctness demo ───────────────────────────────────────
static void demo_parser_correctness() {
    std::printf("=== ITCH 5.0 Parser: Correctness Demo ===\n");

    // Build a tiny stream with one of every message type
    SyntheticStream s;
    // AAPL locate = 1
    s.stock_dir(1, 0,                "AAPL");
    s.add(  100'000'000, 1001, 'B', 500, "AAPL", 1'505'000); // $150.50 bid
    s.add(  200'000'000, 1002, 'S', 300, "AAPL", 1'510'000); // $151.00 ask
    s.add(  300'000'000, 1003, 'B', 200, "AAPL", 1'504'000); // $150.40 bid
    s.exec( 400'000'000, 1002, 100, 9001);                   // partial fill: 100 of 300 at ask
    s.cancel(500'000'000, 1003, 100);                         // cancel 100 of bid 1003
    s.del(  600'000'000, 1001);                               // delete bid 1001
    s.replace(700'000'000, 1002, 2001, 150, 1'512'000);       // replace ask 1002 with new at $151.20

    // Wire up parser callbacks to print what we see
    ItchParser parser;
    int msg_count = 0;

    parser.on_stock_directory = [&](const StockDirectoryMsg& m) {
        std::printf("  [StockDirectory] locate=%u symbol='%s'\n",
            m.stock_locate, m.stock);
        ++msg_count;
    };
    parser.on_add_order = [&](const AddOrderMsg& m) {
        std::printf("  [AddOrder]       ref=%llu %s qty=%u price=%u (=$%.4f)\n",
            (unsigned long long)m.order_ref,
            m.buy_sell == 'B' ? "BUY " : "SELL",
            m.shares, m.price,
            m.price / 10000.0);
        ++msg_count;
    };
    parser.on_order_executed = [&](const OrderExecutedMsg& m) {
        std::printf("  [OrderExecuted]  ref=%llu exec_qty=%u match=%llu\n",
            (unsigned long long)m.order_ref, m.executed_shares,
            (unsigned long long)m.match_number);
        ++msg_count;
    };
    parser.on_order_cancel = [&](const OrderCancelMsg& m) {
        std::printf("  [OrderCancel]    ref=%llu cancelled=%u\n",
            (unsigned long long)m.order_ref, m.cancelled_shares);
        ++msg_count;
    };
    parser.on_order_delete = [&](const OrderDeleteMsg& m) {
        std::printf("  [OrderDelete]    ref=%llu\n",
            (unsigned long long)m.order_ref);
        ++msg_count;
    };
    parser.on_order_replace = [&](const OrderReplaceMsg& m) {
        std::printf("  [OrderReplace]   orig=%llu new=%llu qty=%u price=%u (=$%.4f)\n",
            (unsigned long long)m.orig_order_ref,
            (unsigned long long)m.new_order_ref,
            m.shares, m.price, m.price / 10000.0);
        ++msg_count;
    };

    uint64_t total = parser.parse(s.buf.data(), s.buf.size());
    std::printf("  Parsed %llu messages (%d callbacks fired)\n\n",
        (unsigned long long)total, msg_count);
}

// ── Section 2: full replay through OrderBook ──────────────────────────────────
static void demo_replay_basic() {
    std::printf("=== Replay: Basic Order Book Lifecycle ===\n");

    SyntheticStream s;
    s.stock_dir(1, 0, "AAPL");

    // Build a realistic opening book: 5 bid levels + 5 ask levels
    uint64_t ref = 1000;
    for (int i = 0; i < 5; ++i) {
        // Bids: $150.00, $149.99, $149.98, $149.97, $149.96
        s.add(i * 1'000'000, ref++, 'B',
              static_cast<uint32_t>(100 * (i + 1)), "AAPL",
              static_cast<uint32_t>(1'500'000 - i * 100));
        // Asks: $150.01, $150.02, $150.03, $150.04, $150.05
        s.add(i * 1'000'000 + 500, ref++, 'S',
              static_cast<uint32_t>(80 * (i + 1)), "AAPL",
              static_cast<uint32_t>(1'500'100 + i * 100));
    }

    // An aggressive buy sweeps the top ask level
    s.exec(6'000'000, 1001, 80, 9001); // fill 80 shares at $150.01 (ref 1001 = first ask)

    // Amend a bid (replace)
    s.replace(7'000'000, 1000, ref++, 200, 1'500'500); // move to $150.05

    // Cancel remaining asks
    s.del(8'000'000, 1003);
    s.del(8'000'001, 1005);

    ReplayEngine engine;
    ReplayEngine::Config cfg;
    cfg.symbol      = "AAPL";
    cfg.tick_divisor = 100;
    cfg.print_trades = true;
    cfg.verbose      = true;

    std::printf("Replaying %zu bytes of ITCH data...\n", s.buf.size());
    auto snap = engine.run(s.buf.data(), s.buf.size(), cfg);

    // Print final book state
    std::printf("\nFinal book state:\n");
    std::vector<OrderBook::DepthLevel> bids, asks;
    engine.book().get_depth(bids, asks, 5);
    for (auto& l : asks)
        std::printf("  ASK  price=%u ($%.2f)  qty=%u  orders=%u\n",
            l.price, l.price / 100.0, l.total_qty, l.order_count);
    std::printf("  --- SPREAD ---\n");
    for (auto& l : bids)
        std::printf("  BID  price=%u ($%.2f)  qty=%u  orders=%u\n",
            l.price, l.price / 100.0, l.total_qty, l.order_count);

    auto stats = engine.book().get_stats();
    std::printf("\n  Spread: %u ticks  Mid: $%.2f\n",
        stats.spread, stats.mid / 100.0);
    std::printf("  Bid total qty: %llu  Ask total qty: %llu\n\n",
        (unsigned long long)stats.bid_total_qty,
        (unsigned long long)stats.ask_total_qty);

    engine.print_stats();
    std::printf("\n");
}

// ── Section 3: large-scale synthetic replay with throughput measurement ───────
static void demo_replay_throughput() {
    std::printf("=== Replay: Throughput Benchmark (%u messages) ===\n", 500'000u);

    SyntheticStream s;
    s.stock_dir(1, 0, "AAPL");
    // Noise symbol (should be filtered out)
    s.stock_dir(2, 0, "MSFT");

    uint64_t ref  = 1;
    uint64_t ts   = 1'000'000; // 1ms start
    uint64_t match = 1;

    const uint32_t MID_PRICE = 1'500'000; // $150.00 in ITCH units
    constexpr int  TOTAL     = 500'000;

    for (int i = 0; i < TOTAL; ++i) {
        ts += 1'000; // 1 μs per message

        int r = i % 10;
        if (r < 4) {
            // 40%: add bid
            uint32_t price = MID_PRICE - static_cast<uint32_t>((i % 10) * 100);
            s.add(ts, ref++, 'B', 100, "AAPL", price);
        } else if (r < 8) {
            // 40%: add ask
            uint32_t price = MID_PRICE + static_cast<uint32_t>((i % 10 + 1) * 100);
            s.add(ts, ref++, 'S', 100, "AAPL", price);
        } else if (r == 8 && ref > 50) {
            // 10%: delete an old order
            s.del(ts, ref - 50);
        } else {
            // 10%: add MSFT order (should be filtered — different locate)
            s.add(ts, ref++, 'B', 100, "MSFT", MID_PRICE, 2);
        }
    }

    std::printf("  Stream size: %.2f MB (%zu bytes)\n",
        s.buf.size() / 1e6, s.buf.size());

    ReplayEngine engine;
    ReplayEngine::Config cfg;
    cfg.symbol       = "AAPL";
    cfg.tick_divisor = 100;

    auto snap = engine.run(s.buf.data(), s.buf.size(), cfg);
    engine.print_stats();
    std::printf("\n");
}

// ── Section 4: replay from file (real NASDAQ ITCH data) ──────────────────────
static void demo_replay_from_file(const char* path, const char* symbol) {
    std::printf("=== Replay from File: %s (symbol=%s) ===\n", path, symbol);
    try {
        MmapReader f(path);
        std::printf("  File size: %.3f MB\n", f.size() / 1e6);

        ReplayEngine engine;
        ReplayEngine::Config cfg;
        cfg.symbol       = symbol;
        cfg.tick_divisor = 100;
        cfg.print_trades = false;

        auto snap = engine.run(f.data(), f.size(), cfg);

        engine.print_stats();

        auto stats = engine.book().get_stats();
        std::printf("  Final spread: %u ticks  Mid: $%.2f\n",
            stats.spread, stats.mid / 100.0);
        std::printf("  Orders in book: %zu\n\n", stats.orders_in_flight);

    } catch (const std::exception& e) {
        std::printf("  Skipped: %s\n\n", e.what());
    }
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::printf("╔══════════════════════════════════════════════════════╗\n");
    std::printf("║   Lock-Free Order Book — Day 3: ITCH Replay         ║\n");
    std::printf("╚══════════════════════════════════════════════════════╝\n\n");

    demo_parser_correctness();
    demo_replay_basic();
    demo_replay_throughput();

    // If a file path and symbol are provided, replay real data
    if (argc >= 3) {
        demo_replay_from_file(argv[1], argv[2]);
    } else {
        std::printf("Tip: pass a real ITCH file to replay real market data:\n");
        std::printf("  ./build/replay <path/to/file.itch> <SYMBOL>\n");
        std::printf("  Example: ./build/replay data/S081322-v50.itch AAPL\n\n");
        std::printf("See scripts/download_itch_data.sh for data sources.\n\n");
    }

    std::printf("Done.\n");
    return 0;
}
