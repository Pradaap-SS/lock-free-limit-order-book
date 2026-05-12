#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <unordered_map>

#include "order_book.h"
#include "itch_parser.h"
#include "stats_counter.h"

// Replay engine: feeds a parsed ITCH stream into the OrderBook.
//
// Symbol filtering:
//   ITCH messages carry a 2-byte stock_locate instead of the full symbol on
//   every message. The engine learns the target symbol's locate from the Stock
//   Directory ('R') message at the start of each session, then filters all
//   subsequent messages by locate — one integer comparison per message.
//
// Order tracking:
//   The OrderBook doesn't expose individual order state (by design).
//   The engine maintains its own order_ref → (price, remaining, side) map so
//   it can correctly translate ITCH partial executions ('E') and partial
//   cancels ('X') into OrderBook modify_order() calls.
//
// Price conversion:
//   ITCH prices are uint32_t in units of $0.0001.
//   OrderBook ticks = itch_price / tick_divisor.
//   Default: tick_divisor=100 → 1 tick = $0.01 (cents).
//   Supports stocks up to $999.99 with MAX_PRICE_LEVELS=100,000.

// Config is declared at namespace scope so it can be used as a default
// argument — C++ forbids using a nested struct with default-initialized
// members as a default function argument.
struct ReplayConfig {
    const char* symbol      = "AAPL";  // target symbol (plain, no padding)
    uint32_t tick_divisor   = 100;     // ITCH price units per OrderBook tick
    uint64_t max_messages   = 0;       // 0 = process all messages for symbol
    bool     print_trades   = false;   // print each trade to stdout
    bool     verbose        = false;   // print Add/Cancel/Replace events too
};

class ReplayEngine {
public:
    using Config = ReplayConfig;

    // Run the replay. Returns stats after completion.
    // data/size: a complete ITCH binary buffer (e.g. from MmapReader).
    StatsCounter::Snapshot run(const uint8_t* data, std::size_t size,
                                const Config& cfg = {}) {
        cfg_       = cfg;
        locate_    = 0;
        found_sym_ = false;
        stats_.reset();
        order_tracker_.clear();
        order_tracker_.reserve(1 << 20); // pre-size for 1M orders

        setup_callbacks();

        const auto t0 = std::chrono::steady_clock::now();
        parser_.parse(data, size);
        const auto t1 = std::chrono::steady_clock::now();

        elapsed_sec_ = std::chrono::duration<double>(t1 - t0).count();
        return stats_.snapshot();
    }

    // Access book state after replay
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] OrderBook&       book()       noexcept { return book_; }
    [[nodiscard]] double elapsed_sec()    const noexcept { return elapsed_sec_; }

    void print_stats() const noexcept { stats_.print(elapsed_sec_); }

private:
    // ── Per-order state tracked outside the OrderBook ──────────────────────
    struct OrderState {
        Price    price;
        Quantity remaining;
        Side     side;
    };

    // Convert ITCH price to OrderBook tick
    Price to_tick(uint32_t itch_price) const noexcept {
        Price t = itch_price / cfg_.tick_divisor;
        // Clamp to valid range
        if (t >= MAX_PRICE_LEVELS) t = MAX_PRICE_LEVELS - 1;
        return t;
    }

    // ── Callback wiring ────────────────────────────────────────────────────
    void setup_callbacks() {
        parser_.on_stock_directory = [this](const StockDirectoryMsg& m) {
            if (!found_sym_ && std::strcmp(m.stock, cfg_.symbol) == 0) {
                locate_    = m.stock_locate;
                found_sym_ = true;
            }
        };

        parser_.on_add_order = [this](const AddOrderMsg& m) {
            if (!found_sym_ || m.stock_locate != locate_) return;
            stats_.inc_message();
            if (cfg_.max_messages && stats_.snapshot().messages_processed > cfg_.max_messages)
                return;

            const Price    tick = to_tick(m.price);
            const Quantity qty  = m.shares;
            const Side     side = (m.buy_sell == 'B') ? Side::Buy : Side::Sell;

            if (!book_.add_order(m.order_ref, tick, qty, side)) {
                stats_.inc_rejected();
                return;
            }
            order_tracker_[m.order_ref] = {tick, qty, side};
            stats_.inc_add();

            if (cfg_.verbose)
                std::printf("  ADD  ref=%llu %s qty=%u @%u\n",
                    (unsigned long long)m.order_ref,
                    side == Side::Buy ? "BUY " : "SELL", qty, tick);
        };

        parser_.on_order_executed = [this](const OrderExecutedMsg& m) {
            if (!found_sym_ || m.stock_locate != locate_) return;
            stats_.inc_message();

            auto it = order_tracker_.find(m.order_ref);
            if (it == order_tracker_.end()) return;

            OrderState& os = it->second;
            const Quantity exec = std::min(m.executed_shares, os.remaining);
            os.remaining -= exec;
            stats_.inc_trade(exec);

            if (os.remaining == 0) {
                book_.cancel_order(m.order_ref);
                order_tracker_.erase(it);
                stats_.inc_cancel();
            } else {
                // Partial fill: reduce remaining in-place (preserves priority)
                book_.modify_order(m.order_ref, os.price, os.remaining);
                stats_.inc_executed();
            }

            if (cfg_.print_trades)
                std::printf("  EXEC ref=%llu exec=%u remaining=%u\n",
                    (unsigned long long)m.order_ref, exec, os.remaining);
        };

        parser_.on_order_cancel = [this](const OrderCancelMsg& m) {
            // ITCH 'X': partial cancellation — reduce qty but order stays
            if (!found_sym_ || m.stock_locate != locate_) return;
            stats_.inc_message();

            auto it = order_tracker_.find(m.order_ref);
            if (it == order_tracker_.end()) return;

            OrderState& os = it->second;
            const Quantity cancelled = std::min(m.cancelled_shares, os.remaining);
            os.remaining -= cancelled;

            if (os.remaining == 0) {
                book_.cancel_order(m.order_ref);
                order_tracker_.erase(it);
            } else {
                book_.modify_order(m.order_ref, os.price, os.remaining);
            }
            stats_.inc_cancel();
        };

        parser_.on_order_delete = [this](const OrderDeleteMsg& m) {
            if (!found_sym_ || m.stock_locate != locate_) return;
            stats_.inc_message();

            book_.cancel_order(m.order_ref);
            order_tracker_.erase(m.order_ref);
            stats_.inc_cancel();

            if (cfg_.verbose)
                std::printf("  DEL  ref=%llu\n", (unsigned long long)m.order_ref);
        };

        parser_.on_order_replace = [this](const OrderReplaceMsg& m) {
            if (!found_sym_ || m.stock_locate != locate_) return;
            stats_.inc_message();

            // Capture the side before erasing (can't find after erase)
            Side side = Side::Buy;
            auto it_old = order_tracker_.find(m.orig_order_ref);
            if (it_old != order_tracker_.end()) side = it_old->second.side;

            // Remove old order
            book_.cancel_order(m.orig_order_ref);
            order_tracker_.erase(m.orig_order_ref);

            const Price    tick = to_tick(m.price);
            const Quantity qty  = m.shares;

            if (!book_.add_order(m.new_order_ref, tick, qty, side)) {
                stats_.inc_rejected();
                return;
            }
            order_tracker_[m.new_order_ref] = {tick, qty, side};
            stats_.inc_replace();
        };
    }

    OrderBook    book_;
    ItchParser   parser_;
    StatsCounter stats_;
    Config       cfg_{};

    uint16_t locate_{0};
    bool     found_sym_{false};
    double   elapsed_sec_{0.0};

    std::unordered_map<uint64_t, OrderState> order_tracker_;
};
