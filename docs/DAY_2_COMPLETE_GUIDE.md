# Day 2 Complete Guide: Optimization, Profiling & Order Amendment

> **Who this is for:** Someone who read the Day 1 guide and wants to understand
> every decision made on Day 2 — from why `modify_order` needs two code paths,
> to how `STAGE_TIME` compiles to exactly zero instructions in release mode,
> to what the per-stage latency numbers actually tell you.

---

## Table of Contents

1. [Day 2 Goals & Overview](#1-day-2-goals--overview)
2. [New Trading Concepts](#2-new-trading-concepts)
3. [New Technical Concepts](#3-new-technical-concepts)
4. [Complete Codebase Walkthrough](#4-complete-codebase-walkthrough)
5. [Updated Project Structure](#5-updated-project-structure)
6. [Data Structures Added](#6-data-structures-added)
7. [Build & Run Instructions](#7-build--run-instructions)
8. [Tests Explanation](#8-tests-explanation)
9. [Key Algorithms Implemented](#9-key-algorithms-implemented)
10. [Memory Management Updates](#10-memory-management-updates)
11. [Performance Characteristics](#11-performance-characteristics)
12. [What's Next (Day 3 Preview)](#12-whats-next-day-3-preview)
13. [Common Issues & Debugging](#13-common-issues--debugging)
14. [Glossary](#14-glossary)
15. [Visual Diagrams](#15-visual-diagrams)

---

## 1. Day 2 Goals & Overview

### What were we trying to accomplish?

Day 1 built a correct, fast engine. Day 2 answers the next question every HFT
interviewer asks: **"Now that it works — how do you know *where* time is going,
and what can you do about it?"**

Specifically, Day 2 had five goals:

1. **Add `modify_order`** — the third core operation all exchanges support. Without
   it, traders must cancel + re-submit, wasting two round trips. The fast path
   must preserve time priority (O(1) in-place), the slow path must be correct.

2. **Instrument the hot path** — a `LatencyTracer` that reports p50/p99/p99.9
   for every internal stage (pool_acquire, map_insert, cross_check, match,
   level_insert, cancel_lookup). Zero overhead when disabled.

3. **Expose O(1) book analytics** — `BookStats` (spread, mid, total quantities)
   and `get_depth()` (N-level snapshot) that downstream systems can query without
   slowing the matching engine.

4. **Add hardware-level optimizations** — CPU affinity pinning, real-time thread
   priority, macOS Instruments signposts, and two additional `__builtin_prefetch`
   hints in the match loop.

5. **Measure everything** — 9 benchmarks, a per-stage CSV exporter, and a Python
   visualization pipeline for resume-quality charts.

### What was the result?

```
22/22 unit tests passing
9 benchmarks producing clean numbers
Per-stage tracer showing exactly where nanoseconds go
Python plotter ready to generate histogram + CDF + heatmap PNGs
```

### What was added vs Day 1?

| Dimension | Day 1 | Day 2 |
|---|---|---|
| Operations | add, cancel | + modify (2 paths) |
| Analytics queries | top_of_book | + get_stats, get_depth |
| Unit tests | 9 | **22** (+13 new) |
| Benchmarks | 4 | **9** (+5 new) |
| New headers | 0 | 4 (tracer, affinity, signpost, + order.h change) |
| Hot-path prefetches | 1 | **3** (head + next order + next level) |
| Instruments integration | none | signpost intervals on every operation |

---

## 2. New Trading Concepts

### What is order amendment (modify_order)?

Once an order rests in the book, market participants often need to change it —
not to cancel and re-enter, but to adjust in place. Exchanges call this
"amendment," "replace," or "modify."

**Real-world scenario:** A market maker is quoting 500 shares at $100.50.
A large institutional buyer is filling orders nearby. The market maker wants to
reduce their exposure from 500 to 200 shares without losing their priority
position in the queue. If they cancel and re-submit, they lose their spot and
go to the back of the line. If they *amend in place*, they keep their position.

```
Before amendment:
  Queue at $100.50: [#7: 500sh] → [#9: 300sh] → [#12: 200sh]

After in-place amendment of #7 (500sh → 200sh):
  Queue at $100.50: [#7: 200sh] → [#9: 300sh] → [#12: 200sh]
                       ↑ still first! time priority preserved

After cancel + re-add of #7 (500sh → 200sh):
  Queue at $100.50: [#9: 300sh] → [#12: 200sh] → [#7: 200sh]
                                                      ↑ sent to back
```

> **💡 Key Insight:** This is why `modify_order` has two distinct code paths.
> The in-place path (same price, lower qty) is the valuable one — it's what
> keeps market makers competitive. The re-add path is just a convenience
> wrapper for when priority doesn't matter.

### What is market depth?

The **order book depth** is how the book looks beyond just the best bid/ask.
It shows *multiple* levels, revealing hidden liquidity.

```
DEPTH SNAPSHOT (5 levels each side):

Price   Qty    Orders       |    Price   Qty    Orders
-----   ----   ------       |    -----   ----   ------
99999   800      3         BID    100001  500      2    ASK ← best ask
99998   600      4               100002  700      3
99997  1200      8               100003  900      5
99996   400      2               100005  300      1
99994   200      1               100010  2000    12
        ↑ best bid
```

Market makers and algorithmic traders read depth to estimate:
- How much buying pressure exists (total bid qty)
- Where large orders are hidden (thick levels)
- What will happen if a large aggressive order comes in

### What is spread and mid-price?

- **Spread** = best ask − best bid (in ticks)
- **Mid-price** = (best ask + best bid) / 2

```
Best bid: 99,999    Best ask: 100,001
Spread  = 100,001 - 99,999 = 2 ticks
Mid     = (99,999 + 100,001) / 2 = 100,000
```

Market makers constantly monitor spread — it's their profit margin per share
traded. A narrowing spread means increased competition. Most quoting algorithms
reference the mid-price as the "fair value" of the asset.

### What is a market maker?

A market maker simultaneously posts buy *and* sell orders on both sides of the
book, profiting from the spread. They provide liquidity for other participants.

```
Market maker's state at any moment:
  Resting bid at $99.99 (100 shares)  ← buy side
  Resting ask at $100.01 (100 shares) ← sell side

If someone sells to the market maker: fills the bid at $99.99
If someone buys from the market maker: fills the ask at $100.01
Profit per round-trip: $100.01 - $99.99 = $0.02 per share
```

This is why market makers call `modify_order` thousands of times per second —
they continuously adjust quotes based on price movement, inventory, and risk.

---

## 3. New Technical Concepts

### What is per-stage latency profiling?

Instead of measuring "how long does `add_order` take?" in total, per-stage
profiling measures *each internal step separately*:

```
add_order total: 304 ns
  └─ pool_acquire:   0 ns  (pool is hot in L1 cache)
  └─ map_insert:   125 ns  (hash map write — L2 cache miss occasionally)
  └─ cross_check:    0 ns  (branch almost never taken in passive benchmark)
  └─ level_insert:   0 ns  (linked-list pointer update — always L1 hot)
```

This tells you *where* to optimize. If `map_insert` dominates at 125 ns, you
focus on the hash function or collision resolution, not on the pool or the level.

> **⚠️ Common Mistake:** Looking at only the p50 (median). The p99.9 for
> `cancel_lookup` in the tracer output was 232,458 ns (232 μs!). That's the OS
> interrupting the process and stealing the core. p50 = 83 ns looks fine;
> p99.9 reveals the jitter problem that only CPU pinning can fix.

### What is a STAGE_TIME macro and why is it zero-cost when disabled?

```cpp
// When ENABLE_TRACING is defined:
#define STAGE_TIME(tracer, stage) StageTimer _st{tracer, stage}

// When ENABLE_TRACING is NOT defined:
#define STAGE_TIME(tracer, stage) (void)0
```

`(void)0` is the C++ spelling of "nothing." The compiler removes it entirely.
When `ENABLE_TRACING` is not set, every `STAGE_TIME(...)` call in `order_book.h`
generates zero instructions in the compiled binary. The hot path is unaffected.

This is called a **zero-overhead abstraction** — the instrumentation exists in
source code but costs nothing at runtime unless explicitly enabled.

### What is CPU affinity and why does it reduce latency jitter?

A CPU has multiple cores. By default, the OS scheduler can move your thread
from core 2 to core 5 between any two instructions. When this happens:
- All data in core 2's L1/L2 cache is left behind
- Core 5 starts cold — every memory access is a cache miss
- Latency for the next several thousand operations is 5-10x higher

**CPU affinity** tells the OS "please keep this thread on core 0." The OS treats
it as a hint on macOS (soft binding) and a hard constraint on Linux with `isolcpus`.

```
Without affinity: thread hops between cores → p99.9 latency spikes
With affinity:    thread stays on one core  → L1/L2 cache stays warm

Real data from our tracer output:
  cancel_lookup p50:   83 ns  (L1 warm)
  cancel_lookup p99.9: 232,458 ns  (OS interrupted, cold cache restart)
  ↑ This 2800x ratio is the jitter problem affinity partially solves.
```

### What is a real-time thread priority?

Normal processes can be preempted by the OS at any time — a timer interrupt,
another process, a system call. With real-time priority (macOS:
`THREAD_TIME_CONSTRAINT_POLICY`), the OS promises to give your thread CPU time
within a specified deadline window.

```cpp
// We tell the OS: give this thread 0.5ms of CPU every 1ms period
policy.period      = 1'000'000; // 1ms window
policy.computation =   500'000; // needs 0.5ms per window
policy.preemptible = FALSE;     // don't preempt mid-operation
```

This is why low-latency systems run with real-time priority: it dramatically
reduces the tail latency (p99.9, p99.99) caused by OS scheduling jitter.

### What is an Instruments signpost?

Apple's Instruments profiler shows CPU usage, memory, and custom named intervals
called **signposts**. They appear as colored bars on a timeline:

```
Instruments timeline:
  Time: 0ms ──────────────────────────────── 100ms
             ████  ██████  ████  ██████████
             ^     ^       ^     ^
             add   add     add   cancel_order
             (each bar = duration of one signpost interval)
```

By wrapping our operations with `SIGNPOST_BEGIN`/`SIGNPOST_END`, every
`add_order` and `cancel_order` call appears as a named interval in Instruments.
You can click on any bar to see the call stack at that exact moment.

### What are the new `__builtin_prefetch` hints?

Day 1 had one prefetch: fetch the head order at the current price level.
Day 2 added two more:

```cpp
// Prefetch #1 (Day 1): load the head order before we start the inner loop
__builtin_prefetch(level.head, 0, 3);

// Prefetch #2 (Day 2 NEW): while processing level N, tell CPU to load level N+1
// so it's ready when the outer loop advances
if (aggressor->side == Side::Buy && best + 1 < MAX_PRICE_LEVELS)
    __builtin_prefetch(&(*passive_levels)[best + 1], 0, 1);

// Prefetch #3 (Day 2 NEW): while processing order X, load order X->next
// so it's in cache when the inner loop advances
if (passive->next) __builtin_prefetch(passive->next, 0, 3);
```

The `(ptr, 0, 3)` arguments mean:
- `0` = read access (not write)
- `3` = highest locality hint (keep in L1 cache as long as possible)
- `1` = lower locality hint (keep in L2; we'll need it soon but not immediately)

### What is incremental vs computed statistics?

**Computed (Day 1 approach):** Every time you need `bid_total_qty`, scan all
price levels and sum. O(n) where n = number of active price levels.

**Incremental (Day 2 approach):** Maintain `bid_total_qty_` as a running total.
Every add/cancel/match that changes bid-side quantity also updates the counter.
The query cost is O(1) — just read the variable.

```cpp
// add_order: when a bid order rests:
bid_total_qty_ += o->remaining;

// cancel_order: before removing a bid:
bid_total_qty_ -= o->remaining;

// match(): every fill reduces the passive side's total:
passive_total -= fill;  // where passive_total is bid_ or ask_total_qty_

// modify_order (in-place): delta is the quantity reduction:
bid_total_qty_ -= delta;

// get_stats(): trivially O(1)
s.bid_total_qty = bid_total_qty_;
```

The cost: every operation that touches quantities must maintain the counter.
The benefit: `get_stats()` benchmarks at **1.4 ns** — effectively free.

---

## 4. Complete Codebase Walkthrough

### File: `include/order.h` (updated)

**What changed:** One line added to `EventType`.

```cpp
enum class EventType : uint8_t {
    OrderAck,
    OrderCancel,
    OrderModify,   // NEW: in-place qty reduction (preserves time priority)
    Trade,
    TopOfBookUpdate,
};
```

**Why a new event type?** Downstream systems (risk engines, audit logs, market
data feeds) need to distinguish between:
- A `Cancel` followed by an `Ack` (order was removed and re-added at a new
  price — time priority lost, two messages)
- A single `Modify` (quantity reduced in-place — time priority preserved,
  one message)

This distinction matters for regulatory reporting and for clients reconciling
their order state.

---

### File: `include/latency_tracer.h` (new)

**Purpose:** Measure how long each internal stage of `add_order` and
`cancel_order` takes, independently, with nanosecond resolution.

#### The Stage enum

```cpp
enum class Stage : uint8_t {
    PoolAcquire = 0,  // pool_->acquire() — how fast is free-list pop?
    MapInsert,         // order_map_->insert() — hash + probe + write
    CrossCheck,        // price >= best_ask_? check + branch
    MatchExecute,      // entire match() body
    LevelInsert,       // price_level.push_back() — pointer wiring
    CancelLookup,      // find + remove + erase + release
    ModifyInPlace,     // same-price qty reduction path
    COUNT
};
```

Each stage is an independent measurement. You can see, for example, that
`PoolAcquire` is nearly always 0 ns (the free-list head is always hot in
L1 cache), while `MapInsert` occasionally takes 583 ns (an L2/L3 cache miss
when the hash map slot is cold).

#### The StageBuf

```cpp
struct StageBuf {
    std::unique_ptr<uint64_t[]> data{new uint64_t[CAPACITY]{}};
    std::size_t n{0};

    void record(uint64_t t) noexcept { if (n < CAPACITY) data[n++] = t; }
};
```

Each stage has a pre-allocated buffer of 2 million `uint64_t` tick values.
`record()` is a single bounds check and an array write — about 2 ns.

#### StageTimer — RAII measurement

```cpp
struct StageTimer {
    LatencyTracer& tracer;
    Stage          stage;
    uint64_t       start;

    StageTimer(LatencyTracer& t, Stage s) noexcept
        : tracer(t), stage(s), start(read_cycles()) {}
    ~StageTimer() noexcept { tracer.record(stage, read_cycles() - start); }
};
```

On construction: read the start timestamp.
On destruction: read the end timestamp, compute elapsed, record it.
Usage via macro: `STAGE_TIME(tracer_, Stage::MapInsert)` creates a `StageTimer`
that automatically times until the end of the enclosing scope `{}`.

#### The disabled version

```cpp
#else  // !ENABLE_TRACING

class LatencyTracer {
public:
    void record(Stage, uint64_t) noexcept {}
    void reset() noexcept {}
    void print() const { /* ... */ }
    void dump_csv(const char*) const {}
};

#define STAGE_TIME(tracer, stage) (void)0
```

Every method is either empty or a no-op. The compiler eliminates all of them.
The macro expands to `(void)0` which generates no machine code whatsoever.

#### `dump_csv` for Python plotting

```cpp
void dump_csv(const char* path) const {
    FILE* f = std::fopen(path, "w");
    std::fprintf(f, "stage,ns\n");
    for (uint8_t i = 0; i < static_cast<uint8_t>(Stage::COUNT); ++i) {
        const auto& b = bufs_[i];
        for (std::size_t j = 0; j < b.n; ++j)
            std::fprintf(f, "%s,%.2f\n", kStageNames[i], ticks_to_ns(b.data[j]));
    }
    std::fclose(f);
}
```

This writes every raw sample to a CSV file that `scripts/plot_latency.py` reads
to generate the per-stage heatmap (median, p99, p99.9, p99.99 per stage, shown
as a color-coded grid).

---

### File: `include/cpu_affinity.h` (new)

**Purpose:** Pin threads to specific CPU cores, set real-time scheduling priority,
and query the hardware topology. macOS and Linux implementations in one file.

#### macOS: `pin_thread_to_core`

```cpp
inline bool pin_thread_to_core(int core_id) noexcept {
    thread_affinity_policy_data_t policy{core_id};
    kern_return_t r = thread_policy_set(
        pthread_mach_thread_np(pthread_self()),  // get Mach thread handle
        THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_AFFINITY_POLICY_COUNT);
    return r == KERN_SUCCESS;
}
```

`pthread_mach_thread_np()` converts the POSIX thread ID to a Mach port —
the low-level handle the kernel actually uses. `thread_policy_set` with
`THREAD_AFFINITY_POLICY` asks the scheduler to prefer core `core_id` for
this thread.

**Why "prefer" and not "guarantee"?** macOS doesn't allow user processes to
strictly own a core. That requires `isolcpus` in the kernel boot parameters
(Linux-only). On macOS, the best you can get is a strong hint. You'd need
root + SIP disabled for a hard bind.

#### macOS: `set_realtime_priority`

```cpp
inline bool set_realtime_priority() noexcept {
    thread_time_constraint_policy_data_t policy{};
    policy.period      = 1'000'000; // 1ms in Mach ticks
    policy.computation =   500'000; // needs 0.5ms per period
    policy.constraint  = 1'000'000; // hard deadline = same as period
    policy.preemptible = FALSE;     // don't preempt mid-computation
    kern_return_t r = thread_policy_set(
        pthread_mach_thread_np(pthread_self()),
        THREAD_TIME_CONSTRAINT_POLICY, ...);
    return r == KERN_SUCCESS;
}
```

`THREAD_TIME_CONSTRAINT_POLICY` is macOS's version of soft real-time. The kernel
tries to give the thread `computation` time within each `period`. This reduces
the frequency of OS preemption, which is the primary cause of latency spikes.

#### Apple Silicon: performance vs efficiency cores

```cpp
inline int performance_core_count() noexcept {
    int n = 0;
    std::size_t len = sizeof(n);
    if (sysctlbyname("hw.perflevel0.logicalcpu", &n, &len, nullptr, 0) == 0)
        return n;
    return logical_core_count(); // fallback for Intel
}
```

Apple Silicon (M1, M2, M3...) has two types of cores:
- **P-cores (Performance):** fast, high power — `hw.perflevel0.logicalcpu`
- **E-cores (Efficiency):** slow, low power — `hw.perflevel1.logicalcpu`

For HFT work, you always want to pin to a **P-core**. On an M1 MacBook Air
(4P + 4E), cores 0-3 are P-cores, cores 4-7 are E-cores. `pin_thread_to_core(0)`
pins to the first P-core.

---

### File: `include/signpost.h` (new)

**Purpose:** Wrap Apple's `os_signpost` API so the matching engine's operations
appear as named intervals in the Instruments timeline.

```cpp
#define SIGNPOST_BEGIN(log, name) \
    os_signpost_interval_begin(log, OS_SIGNPOST_ID_EXCLUSIVE, name)
#define SIGNPOST_END(log, name) \
    os_signpost_interval_end(log, OS_SIGNPOST_ID_EXCLUSIVE, name)
```

`OS_SIGNPOST_ID_EXCLUSIVE` means "there is only ever one interval with this name
active at a time" — appropriate for our single-threaded matching engine.

**Critical constraint:** `name` must be a **string literal**, not a variable.
The `os_signpost` macros use `_Static_assert(__builtin_constant_p(fmt))` to
enforce this at compile time. You cannot write:

```cpp
const char* name = "add_order";
SIGNPOST_BEGIN(log, name);  // COMPILE ERROR: name is not a constant
SIGNPOST_BEGIN(log, "add_order");  // OK: string literal is constant
```

This is why `SIGNPOST_SCOPE` is implemented as a macro that defines an inline
struct — the name literal propagates correctly through the macro expansion.

**Non-Apple fallback:** All macros expand to `(void)0` on Linux/Windows. No
conditional compilation needed in your application code.

---

### File: `include/order_book.h` (updated)

**New public API surface:**

```cpp
// New operation
bool modify_order(OrderId id, Price new_price, Quantity new_qty) noexcept;

// New query structs
struct BookStats {
    Price    best_bid, best_ask;
    uint32_t spread;      // best_ask - best_bid (ticks)
    Price    mid;         // (best_bid + best_ask) / 2
    uint64_t bid_total_qty, ask_total_qty;
    std::size_t orders_in_flight;
};

struct DepthLevel {
    Price price;
    Quantity total_qty;
    uint32_t order_count;
};

// New query methods
[[nodiscard]] BookStats get_stats() const noexcept;
void get_depth(std::vector<DepthLevel>& bids_out,
               std::vector<DepthLevel>& asks_out,
               std::size_t n = 5) const noexcept;

// Diagnostics accessor
[[nodiscard]] LatencyTracer& tracer() noexcept;
```

**New private members:**

```cpp
uint64_t bid_total_qty_{0};  // maintained incrementally
uint64_t ask_total_qty_{0};

LatencyTracer tracer_;       // zero-size when disabled
decltype(signpost::make_log("", "")) sp_log_;  // os_log_t handle
```

#### `modify_order` — the two-path implementation

```cpp
inline bool OrderBook::modify_order(OrderId id, Price new_price,
                                     Quantity new_qty) noexcept {
    Order* o = order_map_->find(id);           // O(1) lookup
    if (__builtin_expect(o == nullptr, 0)) return false;

    // ── FAST PATH: same price + quantity reduction ────────────────────────────
    if (new_price == o->price && new_qty < o->remaining) {
        STAGE_TIME(tracer_, Stage::ModifyInPlace);
        const Quantity delta = o->remaining - new_qty;  // how much to subtract

        auto& level = (o->side == Side::Buy) ? (*bids_)[o->price]
                                              : (*asks_)[o->price];
        level.total_quantity -= delta;   // update the price level's running total
        if (o->side == Side::Buy) bid_total_qty_ -= delta;
        else                       ask_total_qty_ -= delta;

        o->remaining = new_qty;  // update the order itself
        o->quantity  = new_qty;  // update original qty too (for display)

        Event mod{.type = EventType::OrderModify, .order_id = id};
        publish(mod);
        return true;
    }

    // ── SLOW PATH: price change or quantity increase ───────────────────────────
    // Loses time priority — this is exchange-standard behavior.
    const Side side = o->side;
    cancel_order(id);                        // publishes Cancel event
    return add_order(id, new_price, new_qty, side);  // publishes Ack event
}
```

**Why does quantity *increase* at the same price also lose priority?**

Every major exchange (NASDAQ, NYSE, CME) treats a quantity increase as a new
order. The reasoning: you're asking for more market impact than before. Other
participants in the queue arrived before you demanded more — they deserve
priority. This is a fairness rule, not a technical limitation.

#### `match()` — three prefetch hints

```cpp
PriceLevel& level = (*passive_levels)[best];

// HINT 1: Load the head order (which we'll access immediately after)
__builtin_prefetch(level.head, 0, 3);

// HINT 2: Load the next price level (which we'll need if this level empties)
if (aggressor->side == Side::Buy && best + 1 < MAX_PRICE_LEVELS)
    __builtin_prefetch(&(*passive_levels)[best + 1], 0, 1);

// HINT 3: Load the next order in this level (while we process the current one)
while (...) {
    Order* passive = level.head;
    if (passive->next) __builtin_prefetch(passive->next, 0, 3);
    // ... process passive ...
}
```

These hints fire the cache prefetch unit in parallel with the computation
that follows. By the time the CPU needs that data, it's already in L1 or L2
cache — avoiding the ~200 cycle stall a cold RAM access would cause.

#### `get_depth` — walking the price array

```cpp
inline void OrderBook::get_depth(std::vector<DepthLevel>& bids_out,
                                  std::vector<DepthLevel>& asks_out,
                                  std::size_t n) const noexcept {
    bids_out.clear();
    bids_out.reserve(n);

    // Walk downward from best_bid — only push non-empty levels
    for (Price p = best_bid_; p > 0 && bids_out.size() < n; --p) {
        const auto& lvl = (*bids_)[p];
        if (!lvl.empty())
            bids_out.push_back({p, lvl.total_quantity, lvl.count});
    }
    // Similarly for asks, walking upward from best_ask_
    ...
}
```

This is O(gap) where gap = price distance between non-empty levels. In a
realistic book with clustered prices, gap is small (1-5 ticks between levels).
In a sparse book, it could scan more. The 5-level depth benchmark showed
**12.6 ns** for 10 resting levels spaced 1 tick apart — essentially free.

---

### File: `CMakeLists.txt` (updated)

**New option:**

```cmake
option(ENABLE_TRACING "Enable per-stage LatencyTracer instrumentation" OFF)
if(ENABLE_TRACING)
    add_compile_definitions(ENABLE_TRACING)
endif()
```

**New target: `latency_bench_traced`**

```cmake
add_executable(latency_bench_traced benchmarks/latency_bench.cpp)
target_compile_definitions(latency_bench_traced PRIVATE ENABLE_TRACING)
target_link_libraries(latency_bench_traced PRIVATE orderbook_lib benchmark::benchmark)
```

This lets you benchmark with tracing on *without* recompiling everything. The
normal `latency_bench` remains fast (no tracing overhead).

**Improved Release flags:**

```cmake
set(CMAKE_CXX_FLAGS_RELEASE
    "-O3 -march=native -DNDEBUG -fno-exceptions -fvectorize -fslp-vectorize")
```

`-fvectorize` enables auto-vectorization (SIMD for loops). `-fslp-vectorize`
enables superword-level parallelism. `-fno-exceptions` reduces code size on the
hot path (exception handling metadata takes space in the binary).

---

### File: `src/main.cpp` (updated)

Seven sections, each a self-contained demo:

| Section | What it shows |
|---|---|
| `demo_affinity()` | P-core count, pin result, RT priority result |
| `demo_smoke_test()` | Day 1 regression: add/match/cancel still correct |
| `demo_modify()` | In-place modify (OrderModify event) vs price amendment (Cancel+Ack) |
| `demo_multilevel()` | Single aggressive buy sweeping 3 ask levels → 3 trades |
| `demo_depth()` | 5-level depth snapshot with `get_depth()` + `get_stats()` |
| `demo_latency()` | 1M-sample manual histogram: add, cancel, modify |
| `demo_tracer()` | Per-stage breakdown (enabled only with `-DENABLE_TRACING=ON`) |

---

### File: `scripts/plot_latency.py` (new)

**Purpose:** Turn the Google Benchmark JSON and the tracer CSV into
publication-quality charts for your README and resume.

Three charts generated:

1. **Latency Histogram** (`latency_histogram.png`) — log-scale X axis, density
   Y axis, one curve per benchmark. Shows the shape of the distribution.

2. **Percentile Bar Chart** (`percentile_bars.png`) — grouped bars showing
   p50/p95/p99/p99.9 for each operation side-by-side. Ideal for README.

3. **Per-Stage Heatmap** (`stage_heatmap.png`) — rows = percentiles
   (p50, p75, p90, p95, p99, p99.9), columns = stages. Color intensity =
   latency. Shows at a glance which stages have the most tail latency.

```
Usage:
  # Install matplotlib (one time)
  pip3 install matplotlib numpy

  # Run benchmarks to get JSON
  ./build/latency_bench --benchmark_format=json --benchmark_out=docs/benchmarks/bench.json

  # Run binary with tracing for stage CSV
  cmake build -DENABLE_TRACING=ON && make -C build order_book
  ./build/order_book  # writes docs/benchmarks/stages.csv

  # Generate all charts
  python3 scripts/plot_latency.py
```

---

### File: `scripts/profile_instruments.sh` (new)

**Purpose:** Record a trace file with Apple Instruments via `xctrace` and
provide a guided walkthrough for reading the flamegraph.

```bash
# Time Profiler (flamegraph):
./scripts/profile_instruments.sh

# Allocation tracing (find any surprise mallocs on hot path):
./scripts/profile_instruments.sh alloc

# Signpost timeline (see add_order intervals):
./scripts/profile_instruments.sh signpost
```

The script:
1. Checks that `xctrace` is available (requires full Xcode, not just CLT)
2. Records for 15 seconds while running the order_book binary
3. Saves a `.trace` bundle to `docs/profiles/`
4. Prints step-by-step instructions for reading the flamegraph
5. Optionally exports a text summary via `xctrace export`

---

### File: `benchmarks/latency_bench.cpp` (updated — 5 new benchmarks)

#### `BM_ModifyOrder_SamePrice` — in-place path timing

```cpp
static void BM_ModifyOrder_SamePrice(benchmark::State& state) {
    OrderBook book;
    constexpr int POOL = 1'000'000;
    for (OrderId i = 1; i <= POOL; ++i)
        book.add_order(i, 49'900, 1000, Side::Buy);  // all at same price

    OrderId id = 1;
    Quantity qty = 999;
    for (auto _ : state) {
        if (id > POOL) { id = 1; qty = 999; }
        book.modify_order(id++, 49'900, qty--);       // in-place: same price, less qty
        if (qty == 0) qty = 999;
    }
    state.SetItemsProcessed(state.iterations());
}
```

Result: **27 ns** — because the in-place path is: 1 hash map lookup (warm
cache), 1 field subtraction, 1 counter update, 1 event push. No pool access,
no list rewiring, no best-price update.

#### `BM_ModifyOrder_DifferentPrice` — re-add path timing

```cpp
for (auto _ : state) {
    book.modify_order(id++, new_price--, 100);  // cancel + re-add at new price
}
```

Result: **622 ns** — this is essentially `cancel_order + add_order` in sequence.
Two full operations: hash map erase + pool release + hash map insert + pool
acquire. The overhead is the two round trips to the hash map and pool.

#### `BM_GetStats` — O(1) analytics query

```cpp
for (auto _ : state)
    benchmark::DoNotOptimize(book.get_stats());
```

Result: **1.4 ns** — reads 6 member variables and performs 2 comparisons.
The `DoNotOptimize` prevents the compiler from eliding the call entirely.

#### `BM_GetDepth5` — 5-level snapshot

```cpp
for (auto _ : state) {
    book.get_depth(bids, asks, 5);
    benchmark::DoNotOptimize(bids.data());
}
```

Result: **12.6 ns** for scanning 5 levels on each side. The levels are all
adjacent in the price array (clustered around 49,990 and 50,010), so this
is sequential memory reads — very cache-friendly.

#### `BM_MixedWorkload` — steady-state realistic simulation

```cpp
// 50% add new + 40% cancel oldest + 10% in-place modify
// Net 10% accumulation keeps pool well within 1M capacity
```

Result: **370 ns CPU time** — this is the most realistic number for a
market-making system. It includes the amortized cost of maintaining the pool,
hash map, and price level lists under continuous churn.

---

### File: `tests/order_book_test.cpp` (updated — 13 new tests)

See [Section 8](#8-tests-explanation) for the full per-test breakdown.

---

## 5. Updated Project Structure

```
lock-free-limit-order-book/
│
├── CMakeLists.txt              ← Updated: ENABLE_TRACING, new target, better flags
│
├── include/
│   ├── order.h                 ← Updated: OrderModify event type added
│   ├── price_level.h           ← Unchanged
│   ├── memory_pool.h           ← Unchanged
│   ├── order_map.h             ← Unchanged
│   ├── spsc_queue.h            ← Unchanged
│   ├── timing.h                ← Unchanged
│   ├── order_book.h            ← Updated: modify_order, BookStats, get_depth,
│   │                                       incremental qty totals, tracer hooks,
│   │                                       signpost intervals, 2 new prefetches
│   ├── latency_tracer.h        ← NEW: per-stage timing, STAGE_TIME macro
│   ├── cpu_affinity.h          ← NEW: pin_thread_to_core, set_realtime_priority
│   └── signpost.h              ← NEW: SIGNPOST_BEGIN/END/SCOPE macros
│
├── src/
│   └── main.cpp                ← Updated: 7 demo sections (affinity, modify,
│                                           multilevel, depth, latency, tracer)
│
├── tests/
│   └── order_book_test.cpp     ← Updated: 22 total tests (was 9, added 13)
│
├── benchmarks/
│   └── latency_bench.cpp       ← Updated: 9 total benchmarks (was 4, added 5)
│
├── scripts/
│   ├── build.sh                ← Unchanged
│   ├── run_bench.sh            ← Unchanged
│   ├── plot_latency.py         ← NEW: matplotlib histogram + CDF + heatmap
│   └── profile_instruments.sh  ← NEW: xctrace recording + guidance
│
├── docs/
│   ├── DAY_1_COMPLETE_GUIDE.md ← Unchanged
│   ├── DAY_2_COMPLETE_GUIDE.md ← This file
│   └── benchmarks/
│       ├── bench.json          ← Generated by latency_bench
│       ├── stages.csv          ← Generated by order_book (ENABLE_TRACING=ON)
│       ├── latency_histogram.png  ← Generated by plot_latency.py
│       ├── percentile_bars.png    ← Generated by plot_latency.py
│       └── stage_heatmap.png      ← Generated by plot_latency.py
│
└── build/
    ├── order_book              ← Updated binary (all 7 demo sections)
    ├── order_book_tests        ← 22 tests
    ├── latency_bench           ← 9 benchmarks (tracing OFF)
    └── latency_bench_traced    ← 9 benchmarks (tracing ON — overhead visible)
```

---

## 6. Data Structures Added

### BookStats — O(1) Market Analytics

**What it is:** A snapshot of the book's most important scalar metrics,
computable in O(1) because all values are maintained incrementally.

```cpp
struct BookStats {
    Price    best_bid{0};          // highest resting buy price
    Price    best_ask{0};          // lowest resting sell price
    uint32_t spread{0};            // best_ask - best_bid (ticks)
    Price    mid{0};               // (best_bid + best_ask) / 2
    uint64_t bid_total_qty{0};     // all remaining shares on bid side
    uint64_t ask_total_qty{0};     // all remaining shares on ask side
    std::size_t orders_in_flight{0}; // orders currently in the pool
};
```

**How `spread` and `mid` are computed in `get_stats()`:**

```cpp
const bool have_bid = !(*bids_)[best_bid_].empty();
const bool have_ask = !(*asks_)[best_ask_].empty();
s.spread = (have_bid && have_ask) ? best_ask_ - best_bid_ : 0;
s.mid    = (have_bid && have_ask) ? (best_bid_ + best_ask_) / 2 : 0;
```

Both sides must be non-empty to compute a meaningful spread or mid. A one-sided
book (only bids or only asks) has no valid spread.

**Memory impact:** Zero extra heap allocation. The struct is returned by value.

---

### DepthLevel — Price Level Snapshot for Depth Queries

**What it is:** One row in a depth-of-market table.

```cpp
struct DepthLevel {
    Price    price{0};        // the price tick
    Quantity total_qty{0};    // total remaining shares at this price
    uint32_t order_count{0};  // number of separate orders at this price
};
```

`get_depth()` fills two `std::vector<DepthLevel>` vectors — one for bids
(high→low) and one for asks (low→high). The caller provides pre-allocated
vectors which are cleared and refilled.

**Why vectors instead of a fixed array?** The caller controls allocation.
A system that polls depth every millisecond can reuse the same two vectors
across calls, amortizing allocation to zero.

---

### LatencyTracer — Per-Stage Timing Database

**What it is:** An array of 7 circular sample buffers (2M entries each),
one per `Stage`. Enabled only in tracing builds.

```
tracer_.bufs_[PoolAcquire]:    [42, 0, 0, 42, 0, 83, ...]  (ticks)
tracer_.bufs_[MapInsert]:      [125, 83, 167, 125, 208, ...] (ticks)
tracer_.bufs_[CrossCheck]:     [0, 0, 0, 0, 0, ...]
tracer_.bufs_[MatchExecute]:   [1250, 1167, 1375, ...]  (when match occurs)
tracer_.bufs_[LevelInsert]:    [0, 0, 42, 0, ...]
tracer_.bufs_[CancelLookup]:   [83, 125, 83, ...]
tracer_.bufs_[ModifyInPlace]:  [0, 42, 0, 0, ...]
```

**Memory:** 7 stages × 2M samples × 8 bytes = **112 MB** when tracing is
enabled. This is why tracing is opt-in (the disabled version has zero overhead
and zero memory).

---

## 7. Build & Run Instructions

### Build variants

**Standard Release (default — no tracing overhead):**
```bash
export PATH="/opt/homebrew/bin:$PATH"
cmake build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
make -C build -j$(sysctl -n hw.ncpu)
```

**With per-stage tracing enabled:**
```bash
cmake build -DENABLE_TRACING=ON
make -C build order_book -j$(sysctl -n hw.ncpu)
./build/order_book    # runs demo_tracer() section showing per-stage breakdown
```

**Debug build (AddressSanitizer + UBSan):**
```bash
cmake build -DCMAKE_BUILD_TYPE=Debug
make -C build order_book_tests -j$(sysctl -n hw.ncpu)
./build/order_book_tests    # ASan will catch any memory bugs
```

### Running the Day 2 demo

```bash
./build/order_book
```

Expected output sections:
```
=== CPU Affinity ===
  Logical cores: 8
  Performance cores: 4
  Pin main thread to core 0: FAILED (need sudo on some systems)
  Set realtime priority: OK

=== modify_order ===
  ...
  modify_order(10, price=50000, qty=400) — in-place reduction:
  MODIFY (in-place): id=10
  Stats: spread=1  mid=50000  bidQty=400  askQty=500  orders=2

=== Multi-Level Matching ===
  Aggressive buy: price=103, qty=900
  TRADE price=100 qty=200
  TRADE price=101 qty=300
  TRADE price=102 qty=400
  Trades executed: 3 (expected 3)

=== Depth Snapshot ===
  Depth (top 5):
    ASK  price=50001  qty=80      orders=1
    ...
    ----  SPREAD  ----
    BID  price=50000  qty=100     orders=1
    ...

=== Per-Stage Latency Tracer ===
Tracing disabled. Rebuild with -DENABLE_TRACING=ON
```

### Generating per-stage data

```bash
# Step 1: rebuild with tracing
cmake build -DENABLE_TRACING=ON && make -C build order_book

# Step 2: run (writes docs/benchmarks/stages.csv)
mkdir -p docs/benchmarks && ./build/order_book

# Step 3: view in terminal
# (already printed by demo_tracer())

# Step 4: generate heatmap PNG
pip3 install matplotlib numpy
python3 scripts/plot_latency.py

# Step 5: turn tracing back off
cmake build -DENABLE_TRACING=OFF && make -C build
```

### Running the full benchmark suite

```bash
./build/latency_bench \
    --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true \
    --benchmark_format=json \
    --benchmark_out=docs/benchmarks/bench.json

# Then generate charts:
python3 scripts/plot_latency.py
```

### CPU pinning for better benchmark results

```bash
# macOS: nice reduces OS scheduling priority of other processes
# (doesn't pin, but reduces jitter)
sudo nice -n -20 ./build/latency_bench \
    --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true

# The benchmark numbers will have lower stddev / CV
```

### Profiling with Instruments

```bash
# Full Xcode required (not just CLT):
./scripts/profile_instruments.sh

# Open the generated trace:
open docs/profiles/time_profiler_*.trace

# For allocation tracing (verify zero hot-path allocs):
./scripts/profile_instruments.sh alloc
```

---

## 8. Tests Explanation

### Day 1 tests (9 — unchanged, serve as regression guard)

These 9 tests from Day 1 run unchanged to ensure Day 2 changes didn't break
anything. If any of these fail after Day 2 changes, something regressed.

---

### Day 2 — `modify_order` tests (6 new)

#### Test 10: ModifyOrder_SamePrice_QuantityReduction_InPlace

```cpp
TEST_F(OrderBookTest, ModifyOrder_SamePrice_QuantityReduction_InPlace) {
    book.add_order(1, 100, 500, Side::Buy);
    clear_events();

    EXPECT_TRUE(book.modify_order(1, 100, 300));

    EXPECT_EQ(count_events(EventType::OrderModify), 1); // single Modify event
    EXPECT_EQ(count_events(EventType::OrderCancel), 0); // NOT a Cancel
    EXPECT_EQ(book.top_of_book().bid_qty, 300u);        // quantity updated
}
```
**What it tests:** The in-place fast path — same price, lower quantity.
**Why it matters:** If this emits a Cancel instead of a Modify, time priority
is lost. This test catches that regression.

---

#### Test 11: ModifyOrder_SamePrice_QuantityIncrease_ReAdds

```cpp
EXPECT_TRUE(book.modify_order(1, 100, 500));  // was 200, now wants 500

EXPECT_EQ(count_events(EventType::OrderCancel), 1);
EXPECT_EQ(count_events(EventType::OrderAck),    1);
```
**What it tests:** Quantity increase at same price triggers the slow path
(cancel + re-add). An increase means "more market impact," which per exchange
rules loses time priority.
**Why it matters:** If `new_qty >= o->remaining` triggered in-place, we'd
have an order claiming to have `new_qty` shares but the price level's
`total_quantity` would be wrong (we only subtracted a delta, but delta would
be negative for an increase).

---

#### Test 12: ModifyOrder_DifferentPrice_CancelAndReAdd

```cpp
book.add_order(1, 100, 300, Side::Buy);
EXPECT_TRUE(book.modify_order(1, 99, 300));  // new price: 99

EXPECT_EQ(book.top_of_book().bid_price, 99u);  // moved
```
**What it tests:** Price change always takes the slow path. The order must
move from `bids_[100]` to `bids_[99]`.

---

#### Test 13: ModifyOrder_NonExistent_ReturnsFalse

```cpp
EXPECT_FALSE(book.modify_order(999, 100, 100));
```
**What it tests:** Trying to modify an order that doesn't exist returns
`false` without crashing or corrupting state.

---

#### Test 14: ModifyOrder_PreservesTimePriority

```cpp
book.add_order(1, 100, 500, Side::Sell); // first in queue
book.add_order(2, 100, 500, Side::Sell); // second
clear_events();

book.modify_order(1, 100, 200); // in-place reduction — should stay first

book.add_order(3, 101, 200, Side::Buy);  // aggressive: matches first in queue

EXPECT_EQ(trade->trade.passive_id, 1u);  // #1 filled, not #2
```
**What it tests:** The core guarantee of the in-place fast path. After
an in-place modify, the order's position in the FIFO queue is unchanged.
**Why it matters:** This is the entire point of the in-place path. If the
order moved to the back after modification, a market maker calling `modify_order`
would lose priority exactly when they least want to.

---

#### Test 15: ModifyOrder_UpdatesBookStats

```cpp
book.add_order(1, 100, 1000, Side::Buy);
EXPECT_EQ(book.get_stats().bid_total_qty, 1000u);

book.modify_order(1, 100, 600); // reduce by 400

EXPECT_EQ(book.get_stats().bid_total_qty, 600u);
```
**What it tests:** The `bid_total_qty_` counter is correctly decremented
by the delta (400) during an in-place modify.
**Why it matters:** If the counter is wrong, `get_stats()` reports stale
data. A risk system using `bid_total_qty` to measure market exposure would
over-estimate by 400 shares.

---

### Day 2 — Multi-level matching tests (3 new)

#### Test 16: MultiLevel_AggressorSweepsTwoAskLevels

```cpp
book.add_order(1, 100, 200, Side::Sell);
book.add_order(2, 101, 300, Side::Sell);
book.add_order(3, 102, 500, Side::Buy);  // sweeps both

EXPECT_EQ(count_events(EventType::Trade), 2);
EXPECT_EQ(book.top_of_book().ask_qty, 0u);
EXPECT_EQ(book.orders_in_flight(), 0u);   // both orders returned to pool
```
**What it tests:** The outer while loop in `match()` correctly advances
`best_ask_` past the first consumed level and matches the second.
**Why it matters:** The most common bug in multi-level matching is failing
to advance the best-price pointer after a level empties, leaving the engine
stuck matching nothing.

---

#### Test 17: MultiLevel_AggressorPartiallyFillsSecondLevel

```cpp
book.add_order(3, 102, 350, Side::Buy);  // fills 200 fully, takes 150 of 300

EXPECT_EQ(count_events(EventType::Trade), 2);
EXPECT_EQ(book.top_of_book().ask_price, 101u);
EXPECT_EQ(book.top_of_book().ask_qty,   150u); // 300 - 150
```
**What it tests:** Partial fill stops matching mid-level, leaving the
remaining quantity correctly in the book.

---

#### Test 18: MultiLevel_AggressorSweepsThreeBidLevels

```cpp
book.add_order(4, 99, 300, Side::Sell);  // aggressive sell sweeps 3 bid levels

EXPECT_EQ(count_events(EventType::Trade), 3);
EXPECT_EQ(book.top_of_book().bid_qty, 0u);
```
**What it tests:** Multi-level matching works symmetrically for the sell
side (descending through bid levels), not just the buy side.

---

### Day 2 — BookStats and get_depth tests (4 new)

#### Test 19: BookStats_SpreadAndMid

```cpp
book.add_order(1, 100, 500, Side::Buy);
book.add_order(2, 103, 300, Side::Sell);

auto s = book.get_stats();
EXPECT_EQ(s.spread, 3u);  // 103 - 100
EXPECT_EQ(s.mid,  101u);  // (100 + 103) / 2, integer division
```
**What it tests:** Spread and mid computation with both sides present.

---

#### Test 20: BookStats_TotalQuantityTracked

```cpp
book.add_order(1, 100, 400, Side::Buy);
book.add_order(2, 100, 200, Side::Buy);
EXPECT_EQ(book.get_stats().bid_total_qty, 600u); // 400 + 200

book.cancel_order(1);
EXPECT_EQ(book.get_stats().bid_total_qty, 200u); // correctly decremented
```

---

#### Test 21: BookStats_TotalQuantityAfterMatch

```cpp
book.add_order(1, 100, 500, Side::Sell);
book.add_order(2, 100, 200, Side::Sell);
// ask_total_qty = 700

book.add_order(3, 101, 300, Side::Buy);  // matches 300 shares

EXPECT_EQ(book.get_stats().ask_total_qty, 400u); // 700 - 300
```
**What it tests:** The `passive_total -= fill` in `match()` correctly
reduces the running total during trade execution.

---

#### Test 22: GetDepth_ReturnsSortedLevels

```cpp
book.add_order(1, 100, 100, Side::Buy);
book.add_order(2,  99, 200, Side::Buy);
book.add_order(3,  98, 300, Side::Buy);
book.add_order(4, 101, 150, Side::Sell);
book.add_order(5, 102, 250, Side::Sell);

std::vector<OrderBook::DepthLevel> bids, asks;
book.get_depth(bids, asks, 3);

EXPECT_EQ(bids[0].price, 100u); // high → low
EXPECT_EQ(bids[1].price,  99u);
EXPECT_EQ(asks[0].price, 101u); // low → high
```

---

## 9. Key Algorithms Implemented

### Algorithm 1: `modify_order` — Two-Path Decision

**Problem:** Given an existing order's ID, change its price or quantity while
minimizing performance impact and correctly signaling priority loss.

**Decision tree:**
```
modify_order(id, new_price, new_qty)
         │
         ▼
    order_map_->find(id)
         │
    ┌────▼────┐
    │ found?  │── NO ──► return false
    └────┬────┘
         │ YES
         ▼
    new_price == o->price
    AND new_qty < o->remaining?
         │
    ┌────▼──────────────────────────────────────────────┐
    │ YES → FAST PATH (in-place)                        │
    │   delta = o->remaining - new_qty                  │
    │   level.total_quantity -= delta    (O(1))         │
    │   bid/ask_total_qty_   -= delta    (O(1))         │
    │   o->remaining = new_qty           (O(1))         │
    │   emit OrderModify event           (O(1))         │
    │   Total: ~5 ns                                    │
    └───────────────────────────────────────────────────┘
         │
    ┌────▼──────────────────────────────────────────────┐
    │ NO → SLOW PATH (cancel + re-add)                  │
    │   cancel_order(id)                 (O(1))         │
    │     → removes from level + map + pool             │
    │     → emits OrderCancel                           │
    │   add_order(id, new_price, new_qty, side)  (O(1)) │
    │     → acquires from pool + inserts to map + level │
    │     → emits OrderAck                              │
    │   Total: ~2× add_order ≈ 600+ ns                  │
    └───────────────────────────────────────────────────┘
```

**Time complexity:** Both paths are O(1). The difference is the constant
factor — about 20× faster for the in-place path.

---

### Algorithm 2: Multi-Level Matching Sweep

**Problem:** An aggressive order's quantity may exceed what's available at one
price level. It must continue matching at the next best price until either
it's fully filled or no more prices meet its limit.

**How it works (buy aggressor example):**

```
Aggressor: price=103, qty=700

State: asks_[100]=200sh, asks_[101]=300sh, asks_[102]=400sh

Outer loop iteration 1:
  best_ask_=100 → asks_[100] non-empty → process
  Fill min(700, 200) = 200 shares
  aggressor.remaining = 500, asks_[100] empty → Trade event

Outer loop iteration 2:
  asks_[100] empty → ++best_ask_ = 101
  best_ask_=101 → asks_[101] non-empty → process
  Fill min(500, 300) = 300 shares
  aggressor.remaining = 200, asks_[101] empty → Trade event

Outer loop iteration 3:
  asks_[101] empty → ++best_ask_ = 102
  best_ask_=102 → asks_[102] non-empty → process
  Fill min(200, 400) = 200 shares
  aggressor.remaining = 0, passive.remaining = 200 → Trade event

Outer loop: aggressor.remaining = 0 → break

Result: 3 trades, aggressor fully filled, asks_[102] has 200 remaining
```

**Complexity:** O(levels consumed + orders within those levels). Each
consumed order is touched exactly once — no re-scanning.

---

### Algorithm 3: Incremental O(1) Total Quantity Maintenance

**Problem:** At any moment, know the total shares on each side of the book.
Scanning all levels is O(active price levels) — too slow.

**Solution:** Maintain two counters and update them on every quantity-changing
operation:

| Operation | Update to `bid_total_qty_` or `ask_total_qty_` |
|---|---|
| Passive add_order (bid) | `bid_total_qty_ += o->remaining` |
| Passive add_order (ask) | `ask_total_qty_ += o->remaining` |
| cancel_order (bid) | `bid_total_qty_ -= o->remaining` |
| cancel_order (ask) | `ask_total_qty_ -= o->remaining` |
| match() fill (passive=ask) | `ask_total_qty_ -= fill` |
| match() fill (passive=bid) | `bid_total_qty_ -= fill` |
| modify_order in-place (bid) | `bid_total_qty_ -= delta` |
| modify_order in-place (ask) | `ask_total_qty_ -= delta` |

**get_stats() cost:** One read per field — truly O(1), benchmarks at 1.4 ns.

**Correctness invariant:** At all times:
```
bid_total_qty_ == sum of o->remaining for all resting bid orders
ask_total_qty_ == sum of o->remaining for all resting ask orders
```
Tests 20 and 21 verify this invariant after cancel and after match, respectively.

---

## 10. Memory Management Updates

### What changed from Day 1?

**New private members on the heap (via inline storage):**
- `uint64_t bid_total_qty_` — 8 bytes, stack (inside OrderBook class, tiny)
- `uint64_t ask_total_qty_` — 8 bytes, stack
- `LatencyTracer tracer_` — either 0 bytes (disabled) or ~112 MB (enabled, heap)
- `decltype(signpost::make_log("", "")) sp_log_` — `os_log_t` = 8 bytes (pointer)

**LatencyTracer memory:**

```
ENABLE_TRACING=OFF:
  LatencyTracer = empty class = 0 bytes (compiler eliminates it entirely)
  STAGE_TIME(...) = (void)0 = 0 machine code instructions

ENABLE_TRACING=ON:
  LatencyTracer owns 7 StageBufs
  Each StageBuf: unique_ptr to 2M uint64_t = 2,000,000 × 8 = 16 MB
  Total: 7 × 16 MB = 112 MB heap
```

This 112 MB only exists in tracing builds. It's on the heap (via `unique_ptr`
inside `StageBuf`), so it doesn't blow the stack. The main binary (tracing OFF)
is unchanged from Day 1 in terms of memory footprint.

### Why `unique_ptr<uint64_t[]>` inside StageBuf instead of `std::vector`?

Two reasons:
1. **Deterministic allocation:** `unique_ptr<uint64_t[]>` allocates exactly once
   at construction time. `std::vector` might reallocate. We never want a
   reallocation during the benchmark run.
2. **No size tracking overhead:** We track `n` ourselves. A `vector` stores
   size + capacity + pointer (24 bytes). We store pointer + size (16 bytes).

---

## 11. Performance Characteristics

### Complete benchmark results (Apple Silicon M-series, Release, 3 repetitions)

| Benchmark | CPU Mean | Throughput | Notes |
|---|---|---|---|
| `add_order` passive | **304 ns** | 3.3M/sec | No match; pool+map+level ops |
| `add_order` aggressive | **334 ns** | 3.0M/sec | 1 match per call; pool pre-warmed |
| `cancel_order` | **368 ns** | 2.7M/sec | map lookup + level remove + pool release |
| `top_of_book` | **1.0 ns** | 1.0B/sec | Two pointer dereferences + checks |
| `modify_order` same price | **27 ns** | 38M/sec | In-place: map lookup + 3 field updates |
| `modify_order` different price | **622 ns** | 1.6M/sec | cancel + re-add: two full operations |
| `get_stats` | **1.4 ns** | 744M/sec | 6 variable reads + 2 comparisons |
| `get_depth (5 levels)` | **12.6 ns** | 79M/sec | 10-level scan, cache-warm, consecutive |
| Mixed workload | **370 ns** | 2.7M/sec | 50% add, 40% cancel, 10% modify |

### Per-stage breakdown (ENABLE_TRACING=ON, 200K operations)

```
Stage            Samples    p50       p99      p99.9    p99.99
-----            -------    ---       ---      -----    ------
pool_acquire     222,856     0ns      42ns      83ns     208ns
map_insert       222,856   125ns     583ns    7500ns    9292ns
cross_check      222,856     0ns    1375ns    1500ns    1542ns
match_execute     22,496  1167ns    1458ns    1542ns    2708ns
level_insert     211,701     0ns      42ns      42ns      83ns
cancel_lookup     62,856    83ns    1542ns  232458ns  289583ns
```

**Reading the per-stage data:**

- **`pool_acquire` p50=0 ns:** The free-list head is always hot in L1. Sub-tick
  (faster than the 42 ns timer resolution). No concern.

- **`map_insert` p99=583 ns, p99.99=9292 ns:** The hash map occasionally
  causes an L2/L3 cache miss (slot is cold). The p99.99 spike to 9.3 μs is
  an OS interrupt landing between the map operations. This is the primary
  target for Day 3 optimization.

- **`cross_check` p50=0 ns:** Passive orders rarely cross — the branch is
  always predicted "not taken." `__builtin_expect` helps the compiler lay out
  the hot path linearly.

- **`match_execute` p50=1167 ns:** Only fires when a match actually occurs
  (~10% of operations). Includes trade event publishing.

- **`level_insert` p50=0 ns:** Tail-pointer update is always L1 hot (we just
  inserted to this level). Nearly always faster than one timer tick.

- **`cancel_lookup` p99.9=232,458 ns:** The 2800x spike over p50=83 ns is
  the OS interrupting the process and cold-starting L1 cache. This is the
  jitter problem — visible only in tail percentiles. CPU pinning and RT
  priority reduce it but cannot eliminate it on macOS (Linux `isolcpus` is
  the real fix).

### Why does `modify_order` (same price) at 27 ns beat even `add_order`?

`add_order` needs: pool acquire + field fill + map insert + level push_back +
best-price update + event publish = ~6 distinct operations.

`modify_order` (in-place) needs: map lookup + 3 arithmetic operations + 1
event push = ~5 operations, all on already-cached data (the order is hot
because we just looked it up). No pool access, no list rewiring.

### What do these numbers mean for a real trading system?

A realistic end-to-end HFT pipeline:

```
Network receive (kernel bypass):   ~500 ns
Protocol decode (FIX/ITCH):        ~200 ns
Risk check:                        ~100 ns
Order book operation:              ~300 ns   ← our engine
Network send:                      ~500 ns
                                   ────────
Total round-trip:                  ~1,600 ns = 1.6 μs
```

Our matching engine contributes about 19% of total latency — within the
right proportion. The goal is never to make the engine faster than the
network; it's to not be the bottleneck.

---

## 12. What's Next (Day 3 Preview)

### What will Day 3 build?

**Primary goal:** NASDAQ ITCH 5.0 protocol parser — read real historical
market data and replay it through our engine at 1000x speed.

**Secondary goals:**
- `modify_order` O(1) best-price update for in-place amendments near the
  best bid/ask
- Lock-free statistics counter for tracking fill rates and throughput
- Multi-symbol support: one `OrderBook` per symbol, sharded across cores

### New concepts you'll learn

| Concept | Why it matters |
|---|---|
| Binary protocol parsing (ITCH) | Real exchanges don't use JSON — binary is 10x smaller and faster |
| `mmap` file I/O | Reading 10 GB of historical data without system call overhead |
| `std::atomic<uint64_t>` statistics | Thread-safe counters without locks |
| Core sharding | Symbols A-M on core 0-3, N-Z on core 4-7 |

### How Day 3 connects to Day 2

```
Day 2 (today):             Day 3:
  modify_order       →       Test with real ITCH amend messages
  get_stats()        →       Feed stats to a risk monitor thread
  LatencyTracer      →       Instrument the ITCH parser stages too
  cpu_affinity.h     →       Pin each symbol's book to its own P-core
  get_depth()        →       Publish as market data to a downstream feed
```

---

## 13. Common Issues & Debugging

### Issue: Tracing build uses 112 MB extra RAM

This is expected. The `LatencyTracer` pre-allocates 7 × 16 MB buffers on
the heap. To reduce: lower `CAPACITY` in `latency_tracer.h`:

```cpp
static constexpr std::size_t CAPACITY = 2'000'000; // change to 500'000
```

This reduces to 7 × 4 MB = 28 MB. For 200K operations (as in `demo_tracer()`),
500K capacity is more than enough.

### Issue: `pin_thread_to_core` returns false

On macOS, this requires elevated privileges in some configurations:

```bash
# Try with sudo
sudo ./build/order_book

# Or check if SIP is blocking thread policy changes
csrutil status
# If "enabled", some thread policies are restricted
```

For benchmarking, affinity failure is not fatal — it just means more jitter
in tail percentiles.

### Issue: `set_realtime_priority` fails

Real-time thread priority on macOS requires the process to have the
`com.apple.security.device.audio-input` entitlement (for audio applications)
or be running as root. For benchmarking, failure is acceptable.

### Issue: `xctrace` not found

```bash
which xctrace
# If not found:
# 1. Install full Xcode from App Store (NOT just Command Line Tools)
# 2. Accept the Xcode license: sudo xcodebuild -license accept
# 3. Select Xcode: sudo xcode-select -s /Applications/Xcode.app

# Verify:
xctrace version
```

### Issue: latency_bench_traced is slower than latency_bench

This is correct and expected. The tracing build adds two `read_cycles()` calls
per stage per operation — about 4-10 ns overhead per stage, or ~28-70 ns total
per `add_order`. Use `latency_bench` (no tracing) for performance numbers to
report on your resume.

### Issue: p99.9 latency is huge (100+ μs) in manual sampling

The manual timer in `main.cpp` captures every operation including OS
preemptions. p99.9 = "1 in 1000 operations hit an OS interrupt." This is
normal on a non-isolated system. The Google Benchmark numbers (300-370 ns)
are more representative because the framework averages millions of iterations.

For the tightest numbers: pin the thread, set RT priority, close other
applications, and use `latency_bench`.

### Issue: `ModifyOrder_SamePrice_QuantityIncrease_ReAdds` test fails

This would mean the in-place condition (`new_qty < o->remaining`) is wrong.
Check that `<` is strict (not `<=`). A modify to exactly the current remaining
quantity is a no-op — the test expects cancel+re-add when `new_qty == remaining`
to avoid ambiguity.

---

## 14. Glossary

**Amendment:** Exchange term for modifying a resting order's price or quantity.
May or may not preserve time priority depending on what changed.

**CPU affinity:** A hint or hard binding that keeps a thread on a specific CPU
core. Reduces cache-cold restarts caused by the OS moving threads.

**Depth of market (DOM):** The full visible resting order book, not just the
best bid/ask. Shows how much liquidity exists at each price level.

**`ENABLE_TRACING`:** CMake preprocessor flag that activates the `LatencyTracer`
instrumentation. When off, all tracing code compiles to nothing.

**False sharing:** When two variables on the same 64-byte cache line are written
by different CPU cores, causing unnecessary cache invalidation. Prevented by
`alignas(64)` on hot variables.

**In-place modify:** Changing an order's quantity at the same price without
removing it from the FIFO queue. Preserves time priority. Only allowed for
quantity reductions in our implementation.

**Incremental counter:** A statistic maintained by updating on every relevant
operation, rather than recomputing by scanning data structures. Makes queries
O(1) instead of O(n).

**Instruments:** Apple's profiling tool (included with Xcode). Shows CPU time,
memory, allocations, and `os_signpost` intervals on a timeline.

**`mach_absolute_time()`:** macOS API for reading the hardware monotonic counter.
Implemented via the commpage (no syscall). ~2 ns.

**Mid-price:** (best_bid + best_ask) / 2. Used as the reference "fair value"
for pricing derivatives and setting quotes.

**`os_signpost`:** Apple API for marking named time intervals that appear in
the Instruments timeline. Requires string literals for interval names (compile-
time constant — runtime strings are not allowed).

**P-core / E-core:** Performance core (fast, high power) and Efficiency core
(slow, low power) on Apple Silicon CPUs. HFT workloads should always run on
P-cores (cores 0-3 on M1, 0-3 on M2, etc.).

**`__builtin_prefetch(ptr, rw, locality)`:** GCC/Clang intrinsic that emits a
hardware cache-prefetch instruction. Loads the cache line at `ptr` into the
CPU cache in the background while other instructions execute.

**`RAII`:** Resource Acquisition Is Initialization. A C++ idiom where a
resource (timer, lock, file handle) is acquired in a constructor and released
in a destructor. Guarantees cleanup even on early return or exception.

**Real-time thread priority:** A scheduling policy where the OS commits to giving
a thread CPU time within a deadline window. Reduces the frequency and duration
of OS preemptions. `THREAD_TIME_CONSTRAINT_POLICY` on macOS, `SCHED_FIFO` on Linux.

**Signpost:** See `os_signpost`.

**Spread:** best_ask − best_bid in price ticks. The market maker's gross profit
per share. Tighter spreads mean more competition among liquidity providers.

**`STAGE_TIME(tracer, stage)`:** Macro that expands to a `StageTimer` RAII
object when `ENABLE_TRACING` is defined, or `(void)0` (nothing) when disabled.

**`xctrace`:** Command-line interface to Apple Instruments, available with full
Xcode. Used to record `.trace` bundles without opening the GUI.

**Zero-overhead abstraction:** A C++ design where an abstraction (class,
template, macro) that is "not used" has zero runtime cost. The disabled
`LatencyTracer` is an example: it exists in source but compiles to nothing.

---

## 15. Visual Diagrams

### modify_order: Two-Path Decision

```
         modify_order(id=7, new_price=100, new_qty=200)
                              │
                      ┌───────▼───────┐
                      │ map.find(7)   │  O(1)
                      └───────┬───────┘
                              │ found: Order at price=100, remaining=500
                              │
            ┌─────────────────▼──────────────────────┐
            │ new_price(100) == o->price(100)?        │
            │ AND new_qty(200) < o->remaining(500)?   │
            └───────────────┬────────────────────────┘
                            │
                   ┌────────▼────────┐
                   │ YES (fast path) │
                   └────────┬────────┘
                            │
             delta = 500 - 200 = 300
             level.total_qty -= 300   (level total: 800 → 500)
             bid_total_qty_  -= 300   (book total: 1200 → 900)
             o->remaining     = 200   (order field updated)
             emit OrderModify event
             return true
             ↑
             ~5 ns, zero pool/list operations, time priority preserved

                   ┌────────▼────────┐
                   │ NO (slow path)  │
                   └────────┬────────┘
                            │ (e.g., new_price=99, or new_qty=600)
             cancel_order(7) → emit Cancel, return to pool
             add_order(7, new_price, new_qty, side) → emit Ack
             ↑
             ~600 ns, two full operations, time priority LOST
```

---

### Multi-Level Match Sweep (Buy Aggressor)

```
Initial book state:
  asks_[100]: [Order#1: 200sh]
  asks_[101]: [Order#2: 300sh]
  asks_[102]: [Order#3: 400sh]
  best_ask_ = 100

New aggressive buy: price=103, qty=700

═══ OUTER LOOP ITERATION 1 ══════════════════════════════════
  best_ask_=100 → asks_[100] not empty
  Prefetch asks_[101] (next level)
  
  INNER LOOP:
    passive = Order#1 (200sh)
    Prefetch passive->next (nullptr, no-op)
    fill = min(700, 200) = 200
    aggressor.remaining = 500
    passive.remaining = 0  → TRADE event (200sh @ 100)
    level.remove(Order#1), map.erase, pool.release
    asks_[100] now empty → inner loop exits

═══ OUTER LOOP ITERATION 2 ══════════════════════════════════
  asks_[100] empty → ++best_ask_ = 101
  asks_[101] not empty → stop advancing
  Prefetch asks_[102] (next level)
  
  INNER LOOP:
    passive = Order#2 (300sh)
    fill = min(500, 300) = 300
    aggressor.remaining = 200
    passive.remaining = 0  → TRADE event (300sh @ 101)
    level.remove(Order#2), map.erase, pool.release
    asks_[101] now empty → inner loop exits

═══ OUTER LOOP ITERATION 3 ══════════════════════════════════
  asks_[101] empty → ++best_ask_ = 102
  asks_[102] not empty → stop advancing
  
  INNER LOOP:
    passive = Order#3 (400sh)
    fill = min(200, 400) = 200
    aggressor.remaining = 0  → TRADE event (200sh @ 102)
    passive.remaining = 200  (not zero, stay in book)
    asks_[102].total_qty -= 200 (400 → 200)
    asks_[102] still has Order#3 with 200sh

═══ OUTER LOOP CHECK ══════════════════════════════════════════
  aggressor.remaining = 0 → break

Final state:
  asks_[102]: [Order#3: 200sh]
  best_ask_ = 102
  Trades: 3, total filled: 700sh
```

---

### Per-Stage Latency Flow Through add_order

```
                      add_order(id, price, qty, side)
                                │
                      ┌─────────▼──────────┐
                      │  SIGNPOST_BEGIN     │  ← appears in Instruments
                      │  "add_order"        │
                      └─────────┬──────────┘
                                │
              ┌─────────────────▼─────────────────┐
              │ STAGE_TIME(PoolAcquire)            │ ─── records: 0-42 ns
              │   o = pool_->acquire()             │     (always L1 hot)
              └─────────────────┬─────────────────┘
                                │
              ┌─────────────────▼─────────────────┐
              │ STAGE_TIME(MapInsert)              │ ─── records: 83-9292 ns
              │   order_map_->insert(id, o)        │     (occasional cache miss)
              └─────────────────┬─────────────────┘
                                │
              ┌─────────────────▼─────────────────┐
              │ STAGE_TIME(CrossCheck)             │ ─── records: 0-1500 ns
              │   price >= best_ask_ ?             │     (almost always 0 for passive)
              │   ┌──────YES──────────────────┐   │
              │   │ STAGE_TIME(MatchExecute)  │   │ ─── records: 1167-2708 ns
              │   │   match(o)               │   │     (only when crossing)
              │   └──────────────────────────┘   │
              └─────────────────┬─────────────────┘
                                │
              ┌─────────────────▼─────────────────┐
              │ STAGE_TIME(LevelInsert)            │ ─── records: 0-83 ns
              │   (*bids_)[price].push_back(o)     │     (tail pointer always L1)
              │   bid_total_qty_ += o->remaining   │
              └─────────────────┬─────────────────┘
                                │
                      ┌─────────▼──────────┐
                      │  SIGNPOST_END       │  ← interval ends in Instruments
                      │  "add_order"        │
                      └─────────┬──────────┘
                                │
                           return true
```

---

### BookStats Incremental Update Points

```
           Events that change bid_total_qty_ or ask_total_qty_:

┌──────────────────────────────────────────────────────────────────────────┐
│  add_order (passive buy rests):  bid_total_qty_ += o->remaining         │
│  add_order (passive sell rests): ask_total_qty_ += o->remaining         │
│  cancel_order (buy):             bid_total_qty_ -= o->remaining         │
│  cancel_order (sell):            ask_total_qty_ -= o->remaining         │
│  match() fill (passive=sell):    ask_total_qty_ -= fill                 │
│  match() fill (passive=buy):     bid_total_qty_ -= fill                 │
│  modify_order in-place (buy):    bid_total_qty_ -= delta                │
│  modify_order in-place (sell):   ask_total_qty_ -= delta                │
└──────────────────────────────────────────────────────────────────────────┘

                     │
                     ▼
            get_stats() → O(1) read
            s.bid_total_qty = bid_total_qty_;   // just a register read
            s.ask_total_qty = ask_total_qty_;

Benchmark: 1.4 ns per get_stats() call — essentially free
```

---

### LatencyTracer Internal Layout (ENABLE_TRACING=ON)

```
LatencyTracer object (on OrderBook's heap via inline member):

bufs_[0] = PoolAcquire:   [42, 0, 0, 42, 0, 83, 0, ...]  n=222,856
bufs_[1] = MapInsert:     [125, 83, 167, 125, ...]         n=222,856
bufs_[2] = CrossCheck:    [0, 0, 0, 0, ...]                n=222,856
bufs_[3] = MatchExecute:  [1167, 1250, 1375, ...]          n=22,496
bufs_[4] = LevelInsert:   [0, 0, 42, 0, ...]               n=211,701
bufs_[5] = CancelLookup:  [83, 125, ...]                   n=62,856
bufs_[6] = ModifyInPlace: [42, 0, 83, ...]                 n=~28,000

Each StageBuf:
  unique_ptr → [u64, u64, u64, ... × 2,000,000]  16 MB
               ↑ pre-allocated at construction, never reallocated

Total memory: 7 × 16 MB = 112 MB (only in tracing builds)

dump_csv() writes all raw values:
  stage,ns
  pool_acquire,0.00
  pool_acquire,41.67
  map_insert,125.00
  ...
  (one row per sample, sorted by stage, then order of recording)
```

---

*End of Day 2 Complete Guide.*

*Next: [Day 3] — NASDAQ ITCH 5.0 protocol parser, multi-symbol sharding, and
replay engine for historical backtesting.*
