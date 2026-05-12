#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

// Lock-free throughput counters for the matching engine.
//
// Design constraints:
//   - Written by a single matching/replay thread (no contention between writers)
//   - Read by a separate monitoring/display thread at any time
//   - Relaxed memory order is sufficient — we're reading telemetry, not
//     synchronizing control flow; slight staleness is acceptable
//   - Each counter on its own cache line to prevent false sharing between
//     the write thread and the read thread invalidating each other's L1 lines

struct alignas(64) StatsCounter {
    // ── Hot counters (written on every message) ──────────────────────────────
    std::atomic<uint64_t> messages_processed{0};

    // ── Per-event counters ────────────────────────────────────────────────────
    // Pad each to its own cache line so the monitoring thread's reads don't
    // invalidate the matching thread's write cache lines.
    alignas(64) std::atomic<uint64_t> orders_added{0};
    alignas(64) std::atomic<uint64_t> orders_cancelled{0};
    alignas(64) std::atomic<uint64_t> orders_replaced{0};
    alignas(64) std::atomic<uint64_t> orders_executed{0};   // partial fills
    alignas(64) std::atomic<uint64_t> orders_rejected{0};   // pool full / price OOB
    alignas(64) std::atomic<uint64_t> trades{0};            // fill events emitted
    alignas(64) std::atomic<uint64_t> shares_traded{0};

    // ── Increment helpers (called by matching/replay thread) ─────────────────
    void inc_message()             noexcept { messages_processed.fetch_add(1, std::memory_order_relaxed); }
    void inc_add()                 noexcept { orders_added.fetch_add(1, std::memory_order_relaxed); }
    void inc_cancel()              noexcept { orders_cancelled.fetch_add(1, std::memory_order_relaxed); }
    void inc_replace()             noexcept { orders_replaced.fetch_add(1, std::memory_order_relaxed); }
    void inc_executed()            noexcept { orders_executed.fetch_add(1, std::memory_order_relaxed); }
    void inc_rejected()            noexcept { orders_rejected.fetch_add(1, std::memory_order_relaxed); }
    void inc_trade(uint64_t qty)   noexcept {
        trades.fetch_add(1, std::memory_order_relaxed);
        shares_traded.fetch_add(qty, std::memory_order_relaxed);
    }

    // ── Snapshot (called by monitoring thread) ────────────────────────────────
    struct Snapshot {
        uint64_t messages_processed;
        uint64_t orders_added;
        uint64_t orders_cancelled;
        uint64_t orders_replaced;
        uint64_t orders_executed;
        uint64_t orders_rejected;
        uint64_t trades;
        uint64_t shares_traded;
    };

    Snapshot snapshot() const noexcept {
        return {
            messages_processed.load(std::memory_order_relaxed),
            orders_added      .load(std::memory_order_relaxed),
            orders_cancelled  .load(std::memory_order_relaxed),
            orders_replaced   .load(std::memory_order_relaxed),
            orders_executed   .load(std::memory_order_relaxed),
            orders_rejected   .load(std::memory_order_relaxed),
            trades            .load(std::memory_order_relaxed),
            shares_traded     .load(std::memory_order_relaxed),
        };
    }

    void print(double elapsed_sec = 0.0) const noexcept {
        auto s = snapshot();
        std::printf("┌─────────────────────────────────────────┐\n");
        std::printf("│ Replay Statistics                       │\n");
        std::printf("├─────────────────────────────────────────┤\n");
        std::printf("│  Messages processed: %16llu    │\n", (unsigned long long)s.messages_processed);
        std::printf("│  Orders added:       %16llu    │\n", (unsigned long long)s.orders_added);
        std::printf("│  Orders cancelled:   %16llu    │\n", (unsigned long long)s.orders_cancelled);
        std::printf("│  Orders replaced:    %16llu    │\n", (unsigned long long)s.orders_replaced);
        std::printf("│  Partial executions: %16llu    │\n", (unsigned long long)s.orders_executed);
        std::printf("│  Orders rejected:    %16llu    │\n", (unsigned long long)s.orders_rejected);
        std::printf("│  Trade events:       %16llu    │\n", (unsigned long long)s.trades);
        std::printf("│  Shares traded:      %16llu    │\n", (unsigned long long)s.shares_traded);
        if (elapsed_sec > 0) {
            double msg_per_sec = s.messages_processed / elapsed_sec;
            std::printf("├─────────────────────────────────────────┤\n");
            std::printf("│  Elapsed:         %14.3f sec       │\n", elapsed_sec);
            std::printf("│  Throughput:      %11.0f msg/sec    │\n", msg_per_sec);
        }
        std::printf("└─────────────────────────────────────────┘\n");
    }

    void reset() noexcept {
        messages_processed.store(0, std::memory_order_relaxed);
        orders_added      .store(0, std::memory_order_relaxed);
        orders_cancelled  .store(0, std::memory_order_relaxed);
        orders_replaced   .store(0, std::memory_order_relaxed);
        orders_executed   .store(0, std::memory_order_relaxed);
        orders_rejected   .store(0, std::memory_order_relaxed);
        trades            .store(0, std::memory_order_relaxed);
        shares_traded     .store(0, std::memory_order_relaxed);
    }
};
