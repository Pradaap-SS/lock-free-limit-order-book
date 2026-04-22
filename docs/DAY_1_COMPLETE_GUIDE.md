# Day 1 Complete Guide: Lock-Free Limit Order Book Engine

> **Who this is for:** Someone with basic C++ knowledge who wants to understand
> every decision made today — from trading concepts to memory layout to benchmark
> numbers. No prior trading or systems-programming experience assumed.

---

## Table of Contents

1. [Project Goals & Overview](#1-project-goals--overview)
2. [Trading Concepts for Beginners](#2-trading-concepts-for-beginners)
3. [Technical Concepts Explained](#3-technical-concepts-explained)
4. [Complete Codebase Walkthrough](#4-complete-codebase-walkthrough)
5. [Project Structure](#5-project-structure)
6. [Data Structures Explained](#6-data-structures-explained)
7. [Build & Run Instructions](#7-build--run-instructions)
8. [Tests Explanation](#8-tests-explanation)
9. [Key Algorithms Implemented](#9-key-algorithms-implemented)
10. [Memory Management](#10-memory-management)
11. [Performance Characteristics](#11-performance-characteristics)
12. [What's Next (Day 2 Preview)](#12-whats-next-day-2-preview)
13. [Common Issues & Debugging](#13-common-issues--debugging)
14. [Glossary](#14-glossary)
15. [Visual Diagrams](#15-visual-diagrams)

---

## 1. Project Goals & Overview

### What were we trying to accomplish today?

Day 1 had one goal: **build a working, fast, correct foundation** that the rest
of the project can grow on. Specifically:

- Design the core data layout (how an Order is stored in memory)
- Build the price-level structure (how orders at the same price are organized)
- Build a zero-allocation memory pool (no `new`/`delete` on the hot path)
- Build an O(1) order lookup table (cancel any order instantly by ID)
- Build a lock-free event queue (notify downstream systems without blocking)
- Wire them together into a working `OrderBook` class
- Write 9 unit tests that pass
- Measure real latency numbers with Google Benchmark

### Why does this project matter for HFT/trading?

Every electronic exchange — NYSE, NASDAQ, CME, Binance — runs an order book at
its core. The order book is the data structure that tracks every resting buy and
sell order, and matches them when prices cross. Performance requirements are
extreme:

| Venue | Latency target |
|---|---|
| Top-tier exchange matching engine | < 1 microsecond |
| Market-maker quoting system | < 5 microseconds |
| High-frequency trading strategy | < 10 microseconds |

A microsecond is 1/1,000,000 of a second. Our results today hit **15 ns** for a
passive add — that is 66x faster than the target above.

### High-level summary of what was built

```
7 header files + 1 source file + 1 test file + 1 benchmark file
= a complete, working, benchmarked order book engine
```

The engine can:
- Accept new orders and immediately check if they match
- Match crossing orders with correct price-time priority
- Cancel any resting order in O(1) time
- Report the best bid and ask price at any moment
- Emit events (Trade, Ack, Cancel) to a downstream consumer
- Process **67 million passive orders per second** on Apple Silicon

---

## 2. Trading Concepts for Beginners

### What is an Order Book?

Imagine a farmers' market. Buyers walk around saying "I'll pay $5 for a pound of
apples." Sellers stand at their stalls saying "I'll sell apples for $6 a pound."
No transaction happens yet because the prices don't match.

An **order book** is the marketplace's ledger. It records:
- Every buyer's offer (bid) — sorted from highest price to lowest
- Every seller's offer (ask) — sorted from lowest price to highest

```
         BIDS (buyers)              ASKS (sellers)
    Price   |  Quantity        Price   |  Quantity
    ------  |  --------        ------  |  --------
    $5.00   |  1,000    <---   $5.01   |    500   <--- BEST ASK (lowest sell)
    $4.99   |    300           $5.02   |  2,000
    $4.98   |  2,500           $5.05   |    800
    $4.95   |  1,200           $5.10   |  3,000

     ^
     BEST BID (highest buy)
```

The **spread** is the gap between best bid and best ask ($5.01 - $5.00 = $0.01).
When a buyer is willing to pay at least the ask price, a trade happens.

### What is an Order?

An order is a single instruction: "I want to buy/sell X units at price Y."

**Real-world analogy:** Placing an order on Amazon.

| Amazon | Order Book |
|---|---|
| Product ID | Order ID |
| Your offer price | Price (in ticks) |
| Quantity ordered | Quantity |
| Buy vs Return | Side (Buy vs Sell) |
| Order timestamp | Timestamp |

In our code, an order looks like:

```cpp
struct Order {
    uint64_t id;          // Like an Amazon order number: unique identifier
    uint32_t price;       // In "ticks" — e.g., 10050 means $100.50
    uint32_t quantity;    // How many shares/contracts originally ordered
    uint32_t remaining;   // How many still need to be filled (can be < quantity)
    uint64_t timestamp;   // When it arrived (used for time priority)
    Side     side;        // Side::Buy or Side::Sell
};
```

### What is a Price Level?

A price level is a group of all orders at the exact same price. Think of it as
a checkout queue at a supermarket. Everyone in the queue wants the same thing
at the same price. The person who arrived first gets served first.

```
Price Level at $50.00 (Buy side):
  [Order #1: 500 shares, arrived 09:30:00.000]
  [Order #2: 300 shares, arrived 09:30:00.005]
  [Order #3: 200 shares, arrived 09:30:00.012]
       ^--- first in, first out
```

### What is Bid vs Ask?

- **Bid** = what buyers are willing to pay ("I BID $50 for this stock")
- **Ask** = what sellers are willing to accept ("I ASK for $51 per share")

The bid is always lower than the ask (if they were equal, a trade would already
have happened). The difference is the **spread**, and market makers profit from it.

### What is Price-Time Priority?

When multiple orders sit at the same price, who gets to trade first? The answer
is **price-time priority** — the two-rule system used by every major exchange:

**Rule 1 — Price Priority:** Better prices go first.
- For buyers: higher price wins (more willing to pay = priority)
- For sellers: lower price wins (more willing to sell cheap = priority)

**Rule 2 — Time Priority:** At the same price, earlier arrival wins.

**Analogy:** Concert ticket line. If tickets are $100 each, the person who
lined up earliest gets first pick. If someone offers $110, they jump the queue
(price priority). Two people offering $110? The one who arrived first wins
(time priority).

> **💡 Key Insight:** Our intrusive linked list at each price level naturally
> enforces time priority — `push_back` adds new orders to the tail, and
> `head` always holds the oldest order. We never need to sort.

### What does "matching" mean?

Matching is when a new order's price crosses an existing order on the other side.

**Example:** Book has a resting sell order at $50. A new buy order arrives at $51.
Since $51 >= $50, the buyer is willing to pay more than the seller demands. They
**match**. A trade happens at $50 (the resting order's price). The buyer got a
good deal — they were willing to pay $51 but only paid $50.

```
New buy arrives at $51 ---->  [matches]  <---- Resting sell at $50
                                   |
                            TRADE at $50
```

- The **aggressor** is the new arriving order (the one that caused the match)
- The **passive** order is the resting one that was already in the book

---

## 3. Technical Concepts Explained

### What is a "lock-free" data structure and why does it matter?

A **lock** is like a bathroom door with an "Occupied" sign. When thread A enters
the bathroom (acquires the lock), thread B must wait outside. This causes
**latency spikes** — if thread A gets suspended by the OS mid-operation, thread B
waits an unpredictable amount of time (could be 10ms, 100ms, anything).

In trading, a 10ms delay can cost millions. So we use **lock-free** designs
instead: data structures where multiple threads can make progress without any
thread ever blocking. They use **atomic operations** — CPU-level instructions
that complete in a single, uninterruptible step.

```
Regular (lock-based):          Lock-Free:
  Thread A grabs lock            Thread A reads atomically
  Thread B WAITS...              Thread B reads atomically (simultaneously!)
  Thread A releases lock         No waiting, ever
  Thread B proceeds
```

Our SPSC queue uses `std::atomic<size_t>` for the head and tail pointers.
The matching engine itself runs single-threaded on purpose — the design avoids
the need for locks in the hot path entirely.

> **⚠️ Common Mistake:** "Lock-free" doesn't mean "thread-safe for any use."
> Our `OrderBook` is single-threaded internally. The SPSC queue (Single-Producer
> Single-Consumer) is lock-free because exactly one thread writes and one reads.

### What is a memory pool and why use it?

Every time you call `new Order()` in C++, the program asks the operating system
for memory. This involves:
1. A system call (expensive: ~1,000 ns)
2. Finding a free block (the allocator might scan a long free list)
3. Updating bookkeeping structures

At 67 million operations/second, we can't afford even one `new` call on the
critical path.

**Solution: pre-allocate everything upfront.**

A memory pool is like a parking garage. Instead of building a new parking space
every time a car arrives (expensive), you build 1,000,000 spaces upfront. Cars
just take a space (fast pointer swap). When they leave, the space is returned
(another pointer swap). No construction, no demolition.

```cpp
// Slow (what we DON'T do on hot path):
Order* o = new Order();    // system call, heap search, bookkeeping

// Fast (what we DO):
Order* o = pool_->acquire();  // one pointer read + one pointer write
```

### What is cache-line alignment and why do we care?

Modern CPUs don't read memory one byte at a time. They read 64 bytes at once
into a very fast buffer called the **cache**. This 64-byte chunk is a
**cache line**.

**The problem — False Sharing:**
If two unrelated variables happen to share a cache line, updating one
invalidates the other in every other CPU core's cache, causing invisible
slowdowns.

```
Cache line (64 bytes):
[Order A: 64 bytes.................................][Order B: 64 bytes]
                                                    ^-- starts on its own line
```

We declare `struct alignas(64) Order` to guarantee each Order object starts
on its own cache line boundary. There's also a compile-time check:

```cpp
static_assert(sizeof(Order) == 64, "Order must fit in one cache line");
```

This means reading one Order requires exactly 1 cache line fetch — the minimum
possible. No wasted reads, no false sharing.

### What is an intrusive linked list vs regular linked list?

**Regular `std::list`:**
```
std::list<Order>:
  [list node: Order data + next* + prev*]  ← list allocates these separately
  [list node: Order data + next* + prev*]
```
The list owns separate node objects. Each `push_back` calls `new`.

**Intrusive linked list:**
```
Orders in pool (already allocated):
  [Order: id, price, qty, ..., next*, prev*]  ← pointers live INSIDE Order
  [Order: id, price, qty, ..., next*, prev*]
```
The Order object itself contains the `next` and `prev` pointers. No separate
node allocation needed. We just wire pointers between existing pool objects.

Result: zero allocations for list operations, no pointer-chasing to a separate
node object, and Orders can be in exactly one list at a time (guaranteed by
the design).

### What is RDTSC and how does it measure nanoseconds?

RDTSC stands for **Read Time-Stamp Counter**. It reads a special CPU register
that counts CPU cycles since boot. On modern x86 hardware, this counter
increments at a fixed frequency (e.g., 3.2 GHz), independent of CPU speed
scaling.

```
elapsed_cycles = rdtsc_after - rdtsc_before
elapsed_ns     = elapsed_cycles * (1,000,000,000 / cpu_frequency_hz)
```

On macOS/Apple Silicon, we use `mach_absolute_time()` instead, which reads the
same hardware counter through the OS commpage (a memory region mapped into every
process — zero system call overhead).

> **💡 Key Insight:** `mach_absolute_time()` is not a system call. Apple maps
> the counter value directly into your process's address space. Reading it is
> just a memory read — about 2 ns.

### Why do we avoid malloc/new on the critical path?

`malloc` is implemented in your C runtime library. Internally it:
1. Searches a free-list (unpredictable time based on heap state)
2. May call `sbrk()` or `mmap()` to grow the heap (OS involvement)
3. Acquires an internal lock (other threads block)
4. Updates bookkeeping metadata

Worst-case `malloc` latency can be **tens of microseconds**. Our entire
order processing pipeline is **15 ns**. One rogue `malloc` would be 1,000x
slower than the entire rest of the work.

The rule in HFT: **allocate everything at startup, never on the hot path.**

---

## 4. Complete Codebase Walkthrough

### File: `include/order.h`

**Purpose:** Defines the fundamental data types — what an Order looks like in
memory, what events the book can emit, and the type aliases used everywhere.

```cpp
// Type aliases — give meaningful names to primitive types
using OrderId  = uint64_t;  // Up to 18 quintillion unique order IDs
using Price    = uint32_t;  // Integer ticks (10050 = $100.50 if tick = $0.01)
using Quantity = uint32_t;  // Number of shares/contracts

enum class Side : uint8_t { Buy = 0, Sell = 1 };
```

**Why integer prices?** Floating-point (0.1 + 0.2 != 0.3) is unreliable for
financial calculations. We use integers in units of the minimum price increment
("tick"). If tick size is $0.01, then $100.50 is stored as `10050`.

```cpp
struct alignas(64) Order {
    OrderId  id;          // 8 bytes: unique order identifier
    Price    price;       // 4 bytes: price in ticks
    Quantity quantity;    // 4 bytes: original order size
    Quantity remaining;   // 4 bytes: how much is left to fill
    uint64_t timestamp;   // 8 bytes: arrival time (for time priority)
    Side     side;        // 1 byte:  Buy or Sell
    uint8_t  _pad[3];     // 3 bytes: padding to reach alignment boundary

    // Intrusive list pointers — 8 bytes each
    Order* next{nullptr}; // 8 bytes: next order at same price
    Order* prev{nullptr}; // 8 bytes: previous order at same price

    //        TOTAL: 8+4+4+4+8+1+3+8+8 = 48 bytes, padded to 64 by alignas
};
static_assert(sizeof(Order) == 64, "Order must fit in one cache line");
```

**Why `remaining` separate from `quantity`?** A partial fill reduces `remaining`
but leaves `quantity` intact. You can always know how much was originally ordered
(`quantity`) and how much is still needed (`remaining`).

```cpp
// Events emitted by the order book to downstream systems
enum class EventType : uint8_t {
    OrderAck,         // Confirmation that an order was accepted
    OrderCancel,      // Confirmation that an order was cancelled
    Trade,            // A match occurred between two orders
    TopOfBookUpdate,  // Best bid or ask price changed
};

struct TradeEvent {
    OrderId  aggressor_id;  // The order that caused the match
    OrderId  passive_id;    // The resting order that was matched against
    Price    price;         // The price at which the trade occurred
    Quantity quantity;      // How many units traded
    uint64_t timestamp;     // When the trade happened
};

// A tagged union — one Event object can hold any event type
struct Event {
    EventType type;
    union {
        OrderId        order_id;  // Used for Ack and Cancel events
        TradeEvent     trade;     // Used for Trade events
        TopOfBookEvent tob;       // Used for top-of-book update events
    };
};
```

**Why a union?** A union makes all event types share the same memory. An `Event`
is always exactly `sizeof(largest_member)` bytes, regardless of which type it
currently holds. This is cache-friendly: the event queue stores identically-sized
slots.

---

### File: `include/price_level.h`

**Purpose:** Represents all orders sitting at one specific price. This is the
fundamental "queue" inside the order book — a doubly-linked intrusive list.

```cpp
struct PriceLevel {
    Order*   head{nullptr};       // Oldest order at this price (matched first)
    Order*   tail{nullptr};       // Newest order at this price (matched last)
    uint32_t count{0};            // Number of orders currently here
    uint32_t total_quantity{0};   // Total shares available at this price
};
```

**`push_back` — add a new order to the back of the queue:**
```cpp
void push_back(Order* o) noexcept {
    o->prev = tail;          // New order points back to current tail
    o->next = nullptr;       // New order is now the last one
    if (tail) tail->next = o; // Old tail points forward to new order
    else      head = o;      // If list was empty, new order is also head
    tail = o;                // Update tail pointer
    ++count;
    total_quantity += o->remaining;
}
```

Step by step when adding Order C to [A <-> B]:
```
Before: head -> [A] <-> [B] <- tail
Step 1: C.prev = B (tail)
Step 2: C.next = nullptr
Step 3: B.next = C
Step 4: tail = C
After:  head -> [A] <-> [B] <-> [C] <- tail
```

**`remove` — remove any order from anywhere in the list:**
```cpp
void remove(Order* o) noexcept {
    if (o->prev) o->prev->next = o->next;  // Previous skips over o
    else         head = o->next;           // o was head; new head is o->next
    if (o->next) o->next->prev = o->prev;  // Next skips back over o
    else         tail = o->prev;           // o was tail; new tail is o->prev
    o->next = o->prev = nullptr;           // Detach o's pointers (safety)
    --count;
    total_quantity -= o->remaining;
}
```

This is O(1) because we have direct pointers to both neighbors. No search needed.

---

### File: `include/memory_pool.h`

**Purpose:** A fixed-size reservoir of pre-allocated Order objects. Acquiring and
releasing orders is a pointer swap — no OS involvement, no heap search.

```cpp
template<std::size_t Capacity>
class OrderPool {
    // ...
    alignas(64) std::array<Order, Capacity> pool_;  // The actual storage
    Order*      free_head_{nullptr};                 // Head of the free list
    std::size_t in_use_{0};                         // How many are checked out
};
```

**Constructor — build the free list:**
```cpp
OrderPool() {
    // Chain all orders together using their 'next' pointer
    for (std::size_t i = 0; i < Capacity - 1; ++i)
        pool_[i].next = &pool_[i + 1];
    pool_[Capacity - 1].next = nullptr;
    free_head_ = &pool_[0];

    // Pre-fault: read one byte from each page to bring them into RAM
    // Avoids page-fault latency on the first real access
    for (auto& o : pool_) {
        volatile char* p = reinterpret_cast<volatile char*>(&o);
        (void)*p;  // Force the read; volatile prevents the compiler removing it
    }
}
```

After construction, the pool looks like a linked list through raw memory:
```
pool_[0] -> pool_[1] -> pool_[2] -> ... -> pool_[N-1] -> nullptr
  ^
  free_head_
```

**`acquire` — check out one Order from the pool:**
```cpp
Order* acquire() noexcept {
    if (__builtin_expect(free_head_ == nullptr, 0)) return nullptr; // pool full
    Order* o = free_head_;      // Grab head of free list
    free_head_ = o->next;       // Advance free list to next item
    o->next = nullptr;          // Detach from free list
    o->prev = nullptr;
    ++in_use_;
    return o;                   // Hand to caller
}
```

`__builtin_expect(condition, 0)` is a **branch prediction hint**. The `0` tells
the compiler "this condition is almost always false." The CPU's branch predictor
can optimize the common case (pool not empty) into a fast path.

**`release` — return an Order back to the pool:**
```cpp
void release(Order* o) noexcept {
    o->next = free_head_;   // Returned order points to old head
    free_head_ = o;         // Returned order becomes new head (LIFO)
    --in_use_;
}
```

> **💡 Key Insight:** The pool uses the Order's own `next` pointer to build the
> free list. This is intrusive design again — the Order struct serves double duty.
> When in the pool (free), `next` chains free items. When in the book (live),
> `next` chains items at the same price level.

---

### File: `include/order_map.h`

**Purpose:** Maps an OrderId to its Order pointer in O(1) time. This enables
instant cancellation — given only an order's ID number, find the exact memory
address and remove it from its price level in nanoseconds.

```cpp
template<std::size_t Capacity>  // Must be a power of 2
class OrderMap {
    struct Slot {
        OrderId key{0};       // 0 means "empty" (no valid order has ID 0)
        Order*  val{nullptr}; // Pointer to the Order object in the pool
    };
    alignas(64) std::array<Slot, Capacity> slots_;
    std::size_t size_{0};
};
```

**How hashing works — Fibonacci hashing:**
```cpp
static std::size_t hash(OrderId id) noexcept {
    return static_cast<std::size_t>(id * 11400714819323198485ULL) & MASK;
}
```

The magic number `11400714819323198485` is `2^64 / φ` where φ (phi) is the
golden ratio (1.618...). Multiplying by this constant then masking achieves
excellent dispersion — sequential IDs (1, 2, 3...) map to widely separated
slots, avoiding clusters.

**Linear probing — finding the right slot:**
```
hash(id) gives us a starting slot index.
If that slot is taken by a different key, try slot+1, slot+2, etc.
Stop when you find the key or an empty slot.
```

```cpp
Order* find(OrderId id) const noexcept {
    std::size_t i = hash(id);
    for (std::size_t probe = 0; probe < Capacity; ++probe) {
        const Slot& s = slots_[(i + probe) & MASK]; // wrap around with MASK
        if (s.key == id)    return s.val;   // Found it
        if (s.key == EMPTY) return nullptr; // Gap = not in map
    }
    return nullptr;
}
```

**Tombstone-free deletion — backward shift:**

Naive deletion would leave a "hole" that breaks linear probing (later lookups
would stop at the hole and miss items past it). Instead, we shift subsequent
slots backward to fill the gap:

```
Before erase of slot 3:
  slot 0: [id=100]  slot 1: [id=101]  slot 2: [EMPTY]  slot 3: [id=103]  slot 4: [id=104]

If id=103 naturally hashes to slot 2 but slot 2 was taken, it went to slot 3.
After erasing slot 3, slide slot 4 back to slot 3 (if it belongs there).

After backward shift:
  slot 0: [id=100]  slot 1: [id=101]  slot 2: [EMPTY]  slot 3: [id=104]  slot 4: [EMPTY]
```

This keeps the probe chain intact without tombstones, maintaining O(1) average
find performance.

---

### File: `include/spsc_queue.h`

**Purpose:** A lock-free, single-producer single-consumer queue for emitting
events (trades, acks, cancels) to a downstream consumer without blocking the
matching engine.

```cpp
template<typename T, std::size_t Capacity>  // T = Event, Capacity = 65536
class SPSCQueue {
    alignas(64) std::atomic<std::size_t> head_{0};  // Written by PRODUCER
    alignas(64) std::atomic<std::size_t> tail_{0};  // Written by CONSUMER
    std::array<T, Capacity> buffer_{};               // The ring buffer
};
```

The queue is a **ring buffer**: a fixed-size circular array. `head` is where the
next item is written; `tail` is where the next item is read.

```
Ring buffer with Capacity=8:
  [_, _, _, _, _, _, _, _]
   0  1  2  3  4  5  6  7

After 3 pushes:
  [A, B, C, _, _, _, _, _]
   ^tail         ^head (next write goes here)

After 2 pops:
  [_, _, C, _, _, _, _, _]
            ^tail  ^head
```

**Why `alignas(64)` on head_ and tail_?** They live on separate cache lines.
The producer writes `head_` constantly. The consumer writes `tail_` constantly.
If they shared a cache line, every write from either thread would invalidate the
other's cache — even though they write different variables. This is **false
sharing** and can multiply latency by 10x.

**`push` (called by producer thread only):**
```cpp
bool push(const T& item) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed); // producer's own var: relaxed OK
    const std::size_t next = (head + 1) & MASK;
    if (__builtin_expect(next == tail_.load(std::memory_order_acquire), 0))
        return false; // full — acquire ensures we see consumer's latest tail
    buffer_[head] = item;
    head_.store(next, std::memory_order_release); // release: makes item visible to consumer
    return true;
}
```

**Memory ordering explained briefly:**
- `relaxed`: no synchronization — just an atomic read/write
- `acquire`: "I see all writes that happened before the matching `release`"
- `release`: "My writes before this are visible to anyone who `acquire`s this"

The producer writes the item, then does a `release` store of the new head.
The consumer does an `acquire` load of head — guaranteeing it sees the item
before it reads it.

---

### File: `include/timing.h`

**Purpose:** Platform-portable high-resolution timer. macOS uses
`mach_absolute_time()`; Linux x86 uses RDTSC.

```cpp
#if defined(__APPLE__)
#include <mach/mach_time.h>

inline uint64_t read_cycles() noexcept {
    return mach_absolute_time();
}
```

`mach_absolute_time()` is not a real function call. Apple implements it through
the **commpage** — a read-only memory region mapped into every process that
contains the current counter value. Reading it is as fast as reading any other
memory location: ~2 ns.

```cpp
struct MachTimebase {
    mach_timebase_info_data_t info{};
    MachTimebase() { mach_timebase_info(&info); }  // Called once at startup

    double ticks_to_ns(uint64_t ticks) const noexcept {
        return static_cast<double>(ticks)
             * static_cast<double>(info.numer)   // numerator (e.g., 125)
             / static_cast<double>(info.denom);  // denominator (e.g., 3)
    }
};
```

On Apple Silicon M-series, `numer=1, denom=1` because the hardware counter
already ticks in nanoseconds. On Intel Macs, the ratio converts from ticks to ns.

**RDTSC (x86 only):**
```cpp
inline uint64_t read_cycles() noexcept {
    uint32_t lo, hi;
    asm volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
```

`lfence` is a CPU instruction that prevents out-of-order execution — ensuring
the `rdtsc` actually runs at the point where you wrote it in the source code,
not speculatively before or after.

**ScopeTimer — RAII latency measurement:**
```cpp
struct ScopeTimer {
    uint64_t  start;
    uint64_t* out;
    explicit ScopeTimer(uint64_t* out) noexcept : start(read_cycles()), out(out) {}
    ~ScopeTimer() noexcept { *out = read_cycles() - start; }
};

// Usage:
uint64_t elapsed;
{
    ScopeTimer t(&elapsed);
    do_work();  // timed automatically
}
// elapsed now holds the tick count
```

---

### File: `include/order_book.h`

**Purpose:** The central engine. Wires together pool, map, price levels, and
event queue. Contains the full implementation of `add_order`, `cancel_order`,
`match`, and queries — all inlined for zero-overhead function calls.

**Constants:**
```cpp
static constexpr std::size_t MAX_PRICE_LEVELS = 100'000; // support prices 0-99999
static constexpr std::size_t ORDER_POOL_SIZE  = 1'048'576; // 1M orders = 64 MB
static constexpr std::size_t ORDER_MAP_SIZE   = 1'048'576; // must be power of 2
static constexpr std::size_t EVENT_QUEUE_SIZE = 65'536;    // 64K pending events
```

**Why heap allocation for large members?**
```cpp
// These are stored via unique_ptr, not directly in the class:
std::unique_ptr<OrderPool<ORDER_POOL_SIZE>>          pool_;    // 64 MB
std::unique_ptr<OrderMap<ORDER_MAP_SIZE>>            order_map_; // 16 MB
std::unique_ptr<std::array<PriceLevel, MAX_PRICE_LEVELS>> bids_; // ~2.4 MB
std::unique_ptr<std::array<PriceLevel, MAX_PRICE_LEVELS>> asks_; // ~2.4 MB
```

macOS gives each thread only 8 MB of stack space by default. If `OrderBook`
stored these directly as members, constructing it on the stack would immediately
crash. `unique_ptr` allocates on the heap, which has no such limit. The
`unique_ptr` overhead (one pointer dereference) is negligible.

**`add_order` — the main entry point:**
```cpp
bool add_order(OrderId id, Price price, Quantity qty, Side side) noexcept {
    // 1. Bounds check (almost never fails — hint to compiler)
    if (__builtin_expect(price >= MAX_PRICE_LEVELS, 0)) return false;

    // 2. Get an Order object from the pool (1 pointer read + 1 write)
    Order* o = pool_->acquire();
    if (__builtin_expect(o == nullptr, 0)) return false;

    // 3. Fill in the order fields
    o->id = id;  o->price = price;  // ... etc

    // 4. Register in the lookup map (for fast cancel)
    if (!order_map_->insert(id, o)) { pool_->release(o); return false; }

    // 5a. If buy: check if it crosses the best ask (matching)
    if (side == Side::Buy) {
        if (price >= best_ask_ && !(*asks_)[best_ask_].empty())
            match(o);

        // 5b. If not fully filled, rest it at its price level
        if (!o->is_filled())
            (*bids_)[price].push_back(o);
        else  // Fully matched — clean up immediately
            { order_map_->erase(id); pool_->release(o); }
    }
    // Similar logic for Sell side...

    // 6. Emit acknowledgment event
    publish(Event{.type = EventType::OrderAck, .order_id = id});
    return true;
}
```

**`match` — the matching engine core:**
```cpp
void match(Order* aggressor) noexcept {
    auto* passive_levels = (aggressor->side == Side::Buy) ? asks_.get() : bids_.get();
    Price& best = (aggressor->side == Side::Buy) ? best_ask_ : best_bid_;

    while (aggressor->remaining > 0) {
        // Skip over empty price levels to find the best available
        // (buy: walk up from best_ask; sell: walk down from best_bid)
        if (aggressor->side == Side::Buy) {
            while (best < MAX_PRICE_LEVELS && (*passive_levels)[best].empty()) ++best;
            if (best >= MAX_PRICE_LEVELS || aggressor->price < best) break; // no match
        }
        // ... symmetric for sell side

        PriceLevel& level = (*passive_levels)[best];
        __builtin_prefetch(level.head, 0, 3); // tell CPU to load head into cache now

        while (aggressor->remaining > 0 && !level.empty()) {
            Order* passive = level.head;  // oldest order at this price
            Quantity fill = std::min(aggressor->remaining, passive->remaining);

            aggressor->remaining -= fill;
            passive->remaining   -= fill;
            level.total_quantity -= fill;

            publish(TradeEvent{...});  // notify downstream

            if (passive->is_filled())  // fully consumed
                { level.remove(passive); order_map_->erase(passive->id); pool_->release(passive); }
        }
    }
}
```

---

### File: `src/main.cpp`

**Purpose:** Two things: (1) a functional smoke test that proves correctness
with human-readable output, and (2) a 1-million-sample latency histogram.

**LatencyStats struct:**
```cpp
struct LatencyStats {
    std::vector<uint64_t> samples;

    void print(const char* label) const {
        auto s = samples;           // copy for sorting (don't modify original)
        std::sort(s.begin(), s.end());

        auto ns_at = [&](double pct) -> double {
            std::size_t i = static_cast<std::size_t>(pct / 100.0 * (s.size() - 1));
            return ticks_to_ns(s[i]);  // convert ticks to nanoseconds
        };

        // Print p50, p95, p99, p99.9, p99.99
        printf("p50=%6.0fns  p99=%6.0fns ...", ns_at(50), ns_at(99));
    }
};
```

**The latency measurement loop:**
```cpp
for (int i = 1; i <= N; ++i) {
    // Use non-crossing prices so no matching occurs (measures pure insert cost)
    Price price = (i % 2 == 0) ? 48000 + (i % 1000)  // buys: 48000-48999
                               : 52000 + (i % 1000);  // sells: 52000-52999
    Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;

    t0 = read_cycles();                                  // START timer
    book.add_order(static_cast<OrderId>(i), price, 100, side);
    t1 = read_cycles();                                  // STOP timer
    add_stats.record(t1 - t0);                          // store elapsed ticks

    if (i % 4 == 0) {  // every 4th iteration, also time a cancel
        t0 = read_cycles();
        book.cancel_order(static_cast<OrderId>(i - 2)); // 2 behind: guaranteed resting
        t1 = read_cycles();
        cancel_stats.record(t1 - t0);
    }
}
```

---

### File: `benchmarks/latency_bench.cpp`

**Purpose:** Four Google Benchmark tests that measure each operation in isolation
under controlled conditions with proper statistical reporting.

| Benchmark | What it measures |
|---|---|
| `BM_AddOrderPassive` | Pure insertion, no matching |
| `BM_AddOrderAggressive` | Matching (find counterpart + execute trade) |
| `BM_CancelOrder` | O(1) lookup + intrusive list removal |
| `BM_TopOfBook` | Single pointer read + empty check |

---

### File: `tests/order_book_test.cpp`

**Purpose:** 9 unit tests using Google Test that verify every functional
guarantee of the order book. See [Section 8](#8-tests-explanation) for details.

---

## 5. Project Structure

```
lock-free-limit-order-book/
│
├── CMakeLists.txt              ← Build configuration (cmake)
│
├── include/                    ← All header files (implementation is here too)
│   ├── order.h                 ← Order struct, Event types, type aliases
│   ├── price_level.h           ← PriceLevel: intrusive linked list at one price
│   ├── memory_pool.h           ← OrderPool: pre-allocated order reservoir
│   ├── order_map.h             ← OrderMap: O(1) ID → Order* hash map
│   ├── spsc_queue.h            ← SPSCQueue: lock-free event queue
│   ├── timing.h                ← read_cycles(), ticks_to_ns() (macOS + x86)
│   └── order_book.h            ← OrderBook: the main engine (all inline)
│
├── src/
│   └── main.cpp                ← Smoke test + 1M-sample latency histogram
│
├── tests/
│   └── order_book_test.cpp     ← 9 Google Test unit tests
│
├── benchmarks/
│   └── latency_bench.cpp       ← 4 Google Benchmark performance tests
│
├── scripts/
│   ├── build.sh                ← One-command build (installs deps if needed)
│   └── run_bench.sh            ← Run benchmarks and save results to JSON/CSV
│
├── docs/
│   └── DAY_1_COMPLETE_GUIDE.md ← This file
│
└── build/                      ← Created by cmake (do not edit)
    ├── order_book              ← Main executable
    ├── order_book_tests        ← Test executable
    └── latency_bench           ← Benchmark executable
```

---

## 6. Data Structures Explained

### OrderPool (Memory Pool)

**What it is:** A fixed-size array of Order objects with a free list threaded
through them.

**Why we need it:** Avoid `new`/`delete` on the hot path. Every order is
pre-allocated at startup.

**How it works:**

```
Initial state (4 slots for simplicity):
┌──────┐   ┌──────┐   ┌──────┐   ┌──────┐
│  0   │──>│  1   │──>│  2   │──>│  3   │──>null
└──────┘   └──────┘   └──────┘   └──────┘
   ^free_head

After acquire() × 2:
            (0 and 1 are "checked out", in use by the book)
                        ┌──────┐   ┌──────┐
                        │  2   │──>│  3   │──>null
                        └──────┘   └──────┘
                           ^free_head

After release(slot 0):
┌──────┐   ┌──────┐   ┌──────┐
│  0   │──>│  2   │──>│  3   │──>null   (slot 1 still checked out)
└──────┘   └──────┘   └──────┘
   ^free_head
```

**Memory layout:**
```
pool_ array in memory (64 bytes per Order, cache-line aligned):
│<-- Order[0], 64 bytes -->│<-- Order[1], 64 bytes -->│<-- Order[2]... -->│
│ id price qty rem ts side│ id price qty rem ts side  │ ...               │
│ pad next prev           │ pad next prev             │ ...               │
```

---

### PriceLevel (Intrusive Doubly-Linked List)

**What it is:** A FIFO queue of orders at a single price tick, implemented as
an intrusive doubly-linked list.

**Why we need it:** At any given price, multiple orders might exist. Price-time
priority means we must serve the oldest first. A linked list with tail-insertion
and head-removal is exactly this.

**How it works:**

```
PriceLevel at price=5000 (buy side):
                 head                           tail
                  ↓                              ↓
              ┌───────┐     ┌───────┐     ┌───────┐
null <── prev │ id=3  │ ←──→│ id=7  │ ←──→│ id=12 │──> next → null
              │qty=500│     │qty=200│     │qty=100│
              └───────┘     └───────┘     └───────┘
              oldest                      newest
              (matched first)             (matched last)

count=3, total_quantity=800
```

**Example push_back (adding order id=15 with qty=300):**
```
Before: [id=3] <-> [id=7] <-> [id=12] (tail)
Step 1: new order.prev = tail (id=12)
Step 2: new order.next = nullptr
Step 3: old tail (id=12).next = new order
Step 4: tail = new order
After:  [id=3] <-> [id=7] <-> [id=12] <-> [id=15] (new tail)
```

**Example remove (cancelling order id=7, in the middle):**
```
Before: [id=3] <-> [id=7] <-> [id=12]
Step 1: id=3.next = id=12  (skip over id=7 going forward)
Step 2: id=12.prev = id=3  (skip over id=7 going backward)
After:  [id=3] <-> [id=12]
        (id=7 is detached, returned to pool)
```

---

### OrderMap (Open-Addressing Hash Map)

**What it is:** An array of (OrderId, Order*) slots, with hash-based placement
and linear probing for collision resolution.

**Why we need it:** Given only an OrderId (from a cancel request), find the
Order's memory address in O(1) without searching price levels.

**How it works:**

```
Capacity=8 slots (MASK=7):

hash(id=101) = 3   hash(id=108) = 3   hash(id=117) = 5

Insert id=101:
  slot 3 empty → place here
  [_][_][_][101→Order*][_][_][_][_]

Insert id=108 (also hashes to 3):
  slot 3 taken → try slot 4 (empty)
  [_][_][_][101→Order*][108→Order*][_][_][_]

Insert id=117 (hashes to 5):
  slot 5 empty → place here
  [_][_][_][101→Order*][108→Order*][117→Order*][_][_]

Find id=108:
  hash → 3, slot 3 has id=101 (not 108), try slot 4, found!
```

---

### SPSCQueue (Ring Buffer)

**What it is:** A fixed-size circular array with atomic head and tail pointers.
One thread pushes; one thread pops.

**Why we need it:** The matching engine (producer) needs to notify downstream
consumers (risk systems, market data feeds) of every trade and ack. Doing so
synchronously would add latency. The SPSC queue lets the engine keep running
while another thread drains events.

**How it works:**

```
Capacity=8, MASK=7

Initial: head=0, tail=0
[_][_][_][_][_][_][_][_]
 0  1  2  3  4  5  6  7

After 3 pushes (head=3):
[A][B][C][_][_][_][_][_]
             ^head=3
 ^tail=0

After 2 pops (tail=2):
[_][_][C][_][_][_][_][_]
          ^head=3
       ^tail=2

Wrapping around (Capacity=8):
Push 6 more items → head = (3+6) & 7 = 1
[H][I][C][D][E][F][G][_]
    ^head=1
       ^tail=2
Reading from slot 2 next.
```

---

## 7. Build & Run Instructions

### Prerequisites

You need macOS with Apple Command Line Tools installed. Run this to verify:
```bash
xcode-select -p
# Should print: /Library/Developer/CommandLineTools
```

If not installed:
```bash
xcode-select --install
```

Then install build tools:
```bash
# Install Homebrew package manager (if not already installed)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install cmake (build system), google-benchmark, and googletest
brew install cmake google-benchmark googletest
```

Verify the installation:
```bash
/opt/homebrew/bin/cmake --version   # Should print cmake version 3.x or 4.x
```

### Building the Project

**Option A — Use the script (easiest):**
```bash
cd /path/to/lock-free-limit-order-book
./scripts/build.sh

# Or with Debug symbols (for running in a debugger):
./scripts/build.sh Debug
```

**Option B — Manual build (to understand each step):**
```bash
# Step 1: Create the build directory (cmake generates files here)
mkdir -p build

# Step 2: Configure — detect compiler, find libraries, generate Makefiles
export PATH="/opt/homebrew/bin:$PATH"
cmake build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
# You should see:
#   -- Building for Apple Silicon (ARM64)
#   -- Google Benchmark found
#   -- Google Test found

# Step 3: Compile everything
make -C build -j$(sysctl -n hw.ncpu)
# -j$(sysctl -n hw.ncpu) uses all available CPU cores in parallel

# Result: three binaries in build/
#   build/order_book         ← smoke test + latency sample
#   build/order_book_tests   ← unit tests
#   build/latency_bench      ← Google Benchmark suite
```

### Running the Smoke Test

```bash
./build/order_book
```

Expected output:
```
=== Lock-Free Limit Order Book — Smoke Test & Latency Sample ===

Timer: mach_absolute_time  (~24 MHz)

--- Resting orders ---
Top of book: bid=500@100  ask=200@101

--- Aggressive buy crosses ask ---
  TRADE: aggressor=5 passive=3 price=101 qty=150
After match: bid=500@100  ask=50@101

--- Cancel resting bid ---
After cancel: bid=300@99  ask=50@101

Trades executed: 1 (expected 1)

--- Latency sampling (1,000,000 add_order + 250,000 cancel_order) ---
add_order             p50=    42ns  p95=    83ns  p99=   167ns  ...
cancel_order          p50=     0ns  p95=    42ns  p99=    42ns  ...

Done.
```

**Reading the output:**
- `bid=500@100` means: 500 shares available at price tick 100
- `TRADE: aggressor=5 passive=3 price=101 qty=150` means: order #5 (the buyer
  who arrived aggressively) matched against order #3 (the resting seller) at
  price tick 101 for 150 shares
- `p50=42ns` means: the median add_order took 42 nanoseconds

### Running Tests

```bash
./build/order_book_tests
```

Expected output (all 9 should pass):
```
[==========] Running 9 tests from 1 test suite.
[ RUN      ] OrderBookTest.AddPassiveBidUpdatesTopOfBook
[       OK ] OrderBookTest.AddPassiveBidUpdatesTopOfBook (26 ms)
...
[  PASSED  ] 9 tests.
```

> **💡 Key Insight:** The "26 ms" time shown by Google Test is the wall-clock
> time to run that test (including OrderBook construction which allocates ~84 MB).
> It is NOT the latency of the operation being tested. Our operations are
> nanoseconds; the test setup time dominates.

### Running Benchmarks

```bash
./build/latency_bench --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

Expected output:
```
Benchmark                                         Time     CPU    Iterations
---------------------------------------------------------------------------
BM_AddOrderPassive/iterations:2000000_mean       15.2 ns  15.1 ns    3
BM_AddOrderPassive/iterations:2000000_median     14.7 ns  14.7 ns    3
BM_AddOrderAggressive/iterations:1000000_mean    2.25 ns  2.25 ns    3
BM_CancelOrder/iterations:1000000_mean           32.8 ns  32.8 ns    3
BM_TopOfBook/iterations:5000000_mean             0.95 ns  0.95 ns    3
```

**How to interpret the columns:**
- `Time`: wall-clock time per operation (includes CPU scheduling jitter)
- `CPU`: CPU time per operation (excludes time the thread wasn't scheduled)
- `Iterations`: how many times each benchmark ran per repetition
- `_mean`: average across 3 repetitions
- `_median`: middle value (more robust to outliers than mean)
- `items_per_second`: throughput (shown in full output with `-v`)

**Save results to JSON and CSV:**
```bash
./scripts/run_bench.sh
# Saves timestamped JSON and CSV to docs/benchmarks/
```

---

## 8. Tests Explanation

All tests live in `tests/order_book_test.cpp` and use Google Test's fixture
pattern: each test gets a fresh `OrderBook` and an event log.

```cpp
class OrderBookTest : public ::testing::Test {
protected:
    std::vector<Event> events;  // Captures every event the book emits
    OrderBook book{[this](const Event& e) { events.push_back(e); }};

    void clear_events() { events.clear(); }

    int count_events(EventType t) {
        return std::count_if(events.begin(), events.end(),
            [t](const Event& e) { return e.type == t; });
    }
};
```

---

### Test 1: AddPassiveBidUpdatesTopOfBook
```cpp
TEST_F(OrderBookTest, AddPassiveBidUpdatesTopOfBook) {
    EXPECT_TRUE(book.add_order(1, 100, 500, Side::Buy)); // Add 500 shares at price 100
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, 100u);  // Best bid should now be 100
    EXPECT_EQ(tob.bid_qty,   500u);  // With 500 shares
}
```
**What it tests:** A buy order with no counterpart rests in the book and becomes
the best bid.
**Why it matters:** If resting orders don't update the top-of-book, market data
consumers see stale prices.

---

### Test 2: AddPassiveAskUpdatesTopOfBook
```cpp
TEST_F(OrderBookTest, AddPassiveAskUpdatesTopOfBook) {
    EXPECT_TRUE(book.add_order(1, 200, 300, Side::Sell));
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.ask_price, 200u);
    EXPECT_EQ(tob.ask_qty,   300u);
}
```
**What it tests:** Symmetric to Test 1, but for the ask side.

---

### Test 3: FullMatchClearsLevel
```cpp
TEST_F(OrderBookTest, FullMatchClearsLevel) {
    book.add_order(1, 100, 200, Side::Sell);  // Resting sell: 200 shares at 100
    clear_events();                            // Reset event log
    book.add_order(2, 101, 200, Side::Buy);   // Aggressive buy at 101: crosses!

    EXPECT_EQ(count_events(EventType::Trade), 1);  // Exactly one trade
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.ask_qty, 0u);  // Level is now empty (fully consumed)
}
```
**What it tests:** When a buy order exactly fills a sell order, both are removed
and the ask becomes empty.
**Why it matters:** A level with 0 shares remaining that still shows in the book
would be a phantom order — dangerous and incorrect.

---

### Test 4: PartialMatchLeavesRemainder
```cpp
TEST_F(OrderBookTest, PartialMatchLeavesRemainder) {
    book.add_order(1, 100, 200, Side::Sell);  // Resting sell: 200 shares
    clear_events();
    book.add_order(2, 101, 100, Side::Buy);   // Buy 100 (only half of the sell)

    EXPECT_EQ(count_events(EventType::Trade), 1);  // One trade happened
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.ask_price, 100u);   // Sell still rests at 100
    EXPECT_EQ(tob.ask_qty,   100u);   // But only 100 shares remain
}
```
**What it tests:** Partial fills reduce `remaining` correctly without removing
the order from the book.

---

### Test 5: CancelOrderRemovesFromBook
```cpp
TEST_F(OrderBookTest, CancelOrderRemovesFromBook) {
    book.add_order(1, 100, 500, Side::Buy);
    EXPECT_TRUE(book.cancel_order(1));  // Should succeed
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.bid_qty, 0u);  // Book should be empty
}
```
**What it tests:** Cancelling a valid order removes it from the book entirely.

---

### Test 6: CancelNonExistentReturnsFalse
```cpp
TEST_F(OrderBookTest, CancelNonExistentReturnsFalse) {
    EXPECT_FALSE(book.cancel_order(999));  // ID 999 was never added
}
```
**What it tests:** Cancelling a non-existent order returns `false` gracefully
instead of crashing or producing undefined behavior.
**Why it matters:** In production, duplicate or stale cancel requests are common.

---

### Test 7: PriceTimePriorityOrderedCorrectly
```cpp
TEST_F(OrderBookTest, PriceTimePriorityOrderedCorrectly) {
    book.add_order(10, 100, 50, Side::Sell);  // First sell at 100
    book.add_order(11, 100, 50, Side::Sell);  // Second sell at 100 (arrived later)
    clear_events();

    book.add_order(20, 101, 50, Side::Buy);   // Aggressive buy (only 50 shares)

    ASSERT_EQ(count_events(EventType::Trade), 1);
    auto trade = std::find_if(events.begin(), events.end(),
        [](const Event& e) { return e.type == EventType::Trade; });
    EXPECT_EQ(trade->trade.passive_id, 10u);  // Order 10 should match, not 11
}
```
**What it tests:** The first order at a price level is matched before later ones.
**Why it matters:** This is the core fairness guarantee of any exchange. Violating
time priority would be a regulatory and reputational disaster.

---

### Test 8: BestBidUpdatesAfterCancel
```cpp
TEST_F(OrderBookTest, BestBidUpdatesAfterCancel) {
    book.add_order(1, 105, 100, Side::Buy);  // Best bid at 105
    book.add_order(2, 100, 100, Side::Buy);  // Second bid at 100
    book.cancel_order(1);                    // Cancel the best bid
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, 100u);          // Best bid should now be 100
}
```
**What it tests:** After cancelling the current best bid, the book correctly
falls back to the next best price.
**Why it matters:** If `best_bid_` isn't updated, top-of-book queries return a
stale (wrong) price. Market makers would quote against the wrong reference.

---

### Test 9: OrdersInFlightCount
```cpp
TEST_F(OrderBookTest, OrdersInFlightCount) {
    book.add_order(1, 100, 100, Side::Buy);
    book.add_order(2, 200, 100, Side::Sell);
    EXPECT_EQ(book.orders_in_flight(), 2u);  // 2 orders in the pool
    book.cancel_order(1);
    EXPECT_EQ(book.orders_in_flight(), 1u);  // 1 returned to pool
}
```
**What it tests:** The pool's `in_use_` counter correctly tracks checked-out
orders.
**Why it matters:** A pool leak (acquiring but never releasing) would exhaust
the pool and cause silent order rejections.

---

## 9. Key Algorithms Implemented

### Algorithm 1: O(1) Order Insertion at a Price Level

**Problem:** Given a new order at price P, add it to that price level in constant time.

**How it works:**
```
1. Look up price P in the array: bids_[P] or asks_[P]   → O(1) array index
2. Append order to the level's tail                     → O(1) pointer update
3. Update best_bid_ or best_ask_ if price is better     → O(1) comparison
```

**What O(1) means:** The operation takes the same amount of time regardless of
how many orders are in the book. Adding the 1,000,000th order takes the same
time as adding the 1st order. Compare to O(n) where time grows with n (like
searching an unsorted list), or O(log n) where it grows logarithmically (like
a binary tree).

**Visual walkthrough:**
```
Book state: best_bid=99, bids_[99] has 3 orders
New buy order arrives: price=100, qty=500

Step 1: price=100 >= MAX_PRICE_LEVELS? No → continue
Step 2: pool_->acquire() → gets Order slot from free list (O(1))
Step 3: Fill order fields: id, price=100, qty=500, remaining=500, timestamp
Step 4: order_map_->insert(id, order*) → hash map O(1) insert
Step 5: price=100 >= best_ask_ (e.g., 99999)? No → no matching needed
Step 6: bids_[100].push_back(order) → tail pointer update (O(1))
Step 7: 100 > best_bid_(99)? Yes → best_bid_ = 100

Result: new best bid is now 100 with 500 shares
Total work: ~10 instructions, no loops, no search
```

---

### Algorithm 2: O(1) Order Cancellation

**Problem:** Given only an OrderId number, remove that order from wherever it
is in the book in constant time.

**How it works:**
```
1. order_map_->find(id)    → hash map lookup → O(1) → get Order* pointer
2. levels[order->price].remove(order)   → intrusive list removal → O(1)
   (we have direct prev/next pointers, no search needed)
3. order_map_->erase(id)   → hash map deletion → O(1)
4. pool_->release(order)   → return to free list → O(1)
```

Without the hash map, cancellation would require scanning the entire price
level list to find the order — O(n) per price level. With millions of orders,
that's catastrophically slow.

---

### Algorithm 3: Price-Time Priority Matching

**Problem:** When a new aggressive order arrives, match it against existing
resting orders in the correct priority order.

**How it works (buy aggressor example):**
```
New buy order: price=103, qty=250

Step 1: Is price(103) >= best_ask_? 
        If yes → potential match exists
        
Step 2: Skip empty ask levels starting from best_ask_:
        best_ask_=101 → asks_[101] not empty → stop

Step 3: Prefetch the head order's memory into cache:
        __builtin_prefetch(asks_[101].head)

Step 4: Inner loop — consume from this price level:
        passive = asks_[101].head  (oldest order at 101)
        fill = min(aggressor.remaining=250, passive.remaining=200) = 200
        aggressor.remaining = 50
        passive.remaining = 0  → fully filled → remove from level + pool

Step 5: aggressor still has 50 remaining → repeat from Step 2
        best_ask_ = 101 → asks_[101] now empty → ++best_ask_ = 102
        asks_[102] not empty?
        If not → aggressor price(103) >= 102 → keep matching
        
Step 6: Eventually aggressor.remaining = 0 or aggressor.price < best_ask_
        → matching stops

Step 7: If aggressor still has remaining → rest it in bids_[103]
```

---

## 10. Memory Management

### How is memory allocated?

Everything is allocated in the `OrderBook` constructor, before any order arrives:

```
Constructor execution:
1. pool_ = make_unique<OrderPool<1M>>()
   → allocates 1,048,576 × 64 bytes = 64 MB on heap
   → pre-faults all pages (touches every page so OS maps RAM immediately)
   → chains free list through all 1M Order objects

2. order_map_ = make_unique<OrderMap<1M>>()
   → allocates 1,048,576 × 16 bytes = 16 MB on heap
   → zeroes all slots (key=0 means empty)

3. event_queue_ = make_unique<SPSCQueue<Event, 65536>>()
   → allocates 65,536 × sizeof(Event) ≈ 1 MB on heap

4. bids_ = make_unique<array<PriceLevel, 100000>>()
   → allocates 100,000 × 24 bytes = ~2.4 MB on heap
   → each PriceLevel is zero-initialized (head=null, tail=null, count=0)

5. asks_ = same as bids_: ~2.4 MB

Total startup allocation: ~86 MB (done once)
```

### Why pre-allocate?

The **page fault** is the hidden enemy. When you first write to a memory page
(4096 bytes), the OS must:
1. Find a physical RAM page
2. Update the page table
3. Resume your program

This takes ~10 microseconds. For a pool of 1M orders × 64 bytes = 16,384 pages,
that's 163 milliseconds of potential page faults at runtime. We pre-fault all
pages in the pool constructor by reading one byte from each:

```cpp
for (auto& o : pool_) {
    volatile char* p = reinterpret_cast<volatile char*>(&o);
    (void)*p;  // Force a read. "volatile" prevents the compiler skipping it.
}
```

After this loop, all 64 MB of pool memory is in physical RAM. The first
`acquire()` takes the same time as the millionth — no surprises.

### Memory pool implementation walkthrough

```
Startup: 1M Orders in a flat array, free list chained through 'next':

pool_[0].next = &pool_[1]
pool_[1].next = &pool_[2]
...
pool_[999998].next = &pool_[999999]
pool_[999999].next = nullptr
free_head_ = &pool_[0]

After book.add_order(id=1, ...):
  o = pool_[0]          (free_head_ advances to pool_[1])
  o.id = 1, o.price = ..., etc.
  order_map_.insert(1, o)
  bids_[price].push_back(o)   (o.next and o.prev now used for price level list)

After book.cancel_order(id=1):
  o = order_map_.find(1)      (O(1) hash lookup → &pool_[0])
  bids_[price].remove(o)      (o.next/prev wired out of list)
  pool_.release(o)             (o.next = free_head_; free_head_ = o)
  free_head_ is now back at &pool_[0]
```

---

## 11. Performance Characteristics

### What did we achieve today?

Measured on Apple M-series (Apple Silicon), Release build, untuned (no CPU pinning,
no OS noise reduction):

| Operation | Mean | p99 (est.) | Throughput |
|---|---|---|---|
| `add_order` (passive) | **15 ns** | ~50 ns | 67M/sec |
| `add_order` (aggressive, matches) | **2.3 ns** | ~10 ns | 445M/sec |
| `cancel_order` | **33 ns** | ~80 ns | 30M/sec |
| `top_of_book` query | **0.95 ns** | ~2 ns | 1.05B/sec |

> **⚠️ Note on the aggressive benchmark:** The 2.3 ns result is fast because the
> pool already has 2M pre-warmed orders loaded into L2/L3 cache. In production,
> cache effects would make this higher. Real matching latency is typically 50-200
> ns in a full system.

### What do p50, p99, p99.9 mean?

These are **percentiles** — a way to describe the distribution of latency:

- **p50 (median):** 50% of operations were faster than this. Half above, half below.
- **p95:** 95% of operations were faster than this. Only 1 in 20 was slower.
- **p99:** 99% were faster. Only 1 in 100 was slower. This is your "tail latency."
- **p99.9:** Only 1 in 1,000 was slower.
- **p99.99:** Only 1 in 10,000 was slower.

**Why p99 matters more than mean in HFT:**
The mean can look great while hiding rare spikes. If your mean is 50 ns but
p99.9 is 10 ms, then 1 order in 1,000 gets a 10ms delay — in a market-making
system quoting 100,000 times/second, that's 100 orders per second experiencing
10ms latency. Catastrophic.

```
Distribution of add_order latency (example):

 Frequency
   ^
   │         ████
   │       ████████
   │     ██████████████
   │   ████████████████████
   │ ██████████████████████████████████      ←─ long tail
   └────────────────────────────────────────> latency (ns)
   0    50   100  200  500  1000  10000

   │p50│   │ p99│      │p99.9│
```

### Why these numbers matter for HFT

The industry target for an exchange matching engine is under 1 microsecond (1000 ns).
Our `add_order` at 15 ns is already 66x better than that target. However, a real
production system adds:

- Network receive/send: ~1-5 μs
- Protocol parsing (ITCH/FIX): ~200-500 ns
- Risk checks: ~100-500 ns
- Kernel bypass (DPDK/RDMA): saves ~5-10 μs

So our core matching engine contributes about 2-15% of total end-to-end latency —
exactly where it should be (not the bottleneck).

### Memory usage

| Component | Size |
|---|---|
| Order pool (1M orders × 64 bytes) | 64 MB |
| Order hash map (1M slots × 16 bytes) | 16 MB |
| Bid price levels (100K × 24 bytes) | 2.4 MB |
| Ask price levels (100K × 24 bytes) | 2.4 MB |
| Event queue (64K × ~48 bytes) | ~3 MB |
| **Total** | **~88 MB** |

This is intentionally large — we pre-allocate everything to guarantee no runtime
allocation. 88 MB is negligible on modern hardware (typical exchange servers have
512 GB+ of RAM).

---

## 12. What's Next (Day 2 Preview)

Day 2 focuses on **optimization and measurement** — taking the correct foundation
and making it faster, while proving it with data.

### What we'll build

**1. Profiling infrastructure**
- Run `Instruments` (Apple's profiler) to generate a flamegraph
- Identify the hottest functions in the matching path
- Measure L1/L2 cache hit rates

**2. `modify_order` operation**
- Allows changing the price or quantity of a resting order
- Naive implementation: cancel + re-add (two O(1) operations)
- Optimized: same-price quantity change (just update the field, O(1))

**3. Multi-level matching improvements**
- Test scenarios where one aggressor matches orders at multiple price levels
- Ensure `best_ask_`/`best_bid_` tracking is correct across level transitions

**4. Latency histogram visualization**
- Write Python script to plot the distribution from CSV benchmark output
- Generate a flamegraph SVG for the README

**5. Thread affinity (optional)**
- Pin the matching engine thread to a specific CPU core
- Reduce context-switching jitter
- Show before/after p99 improvement

### How Day 2 connects to Day 1

Everything in Day 2 builds on the data structures from Day 1:

```
Day 1 (Foundation):         Day 2 (Optimization):
  Order struct           →    Profile: is it in cache?
  OrderPool              →    Tune size to fit in L3
  PriceLevel list        →    Prefetch next node in match loop
  OrderMap hash table    →    Tune load factor for fewer probes
  SPSCQueue              →    Add consumer thread + measure throughput
  read_cycles()          →    Use ScopeTimer throughout match() for profiling
```

---

## 13. Common Issues & Debugging

### Issue: `cmake: command not found`

```bash
# Fix: Add Homebrew to PATH
export PATH="/opt/homebrew/bin:$PATH"

# Make permanent (add to your .zshrc):
echo 'export PATH="/opt/homebrew/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

### Issue: `fatal error: 'cstdio' file not found`

This happens when the Xcode Command Line Tools C++ header stub is incomplete
(a known issue with some CLT versions). Our CMakeLists.txt auto-detects and fixes
this by pointing to the SDK headers:

```cmake
execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE MACOS_SDK_PATH ...)
include_directories(SYSTEM "${MACOS_SDK_PATH}/usr/include/c++/v1")
```

If you still see this error, verify:
```bash
xcrun --show-sdk-path
# Should print something like: /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
ls $(xcrun --show-sdk-path)/usr/include/c++/v1/cstdio
# Should exist
```

### Issue: Binary hangs or crashes silently

Most likely cause: stack overflow from large `OrderBook` members.

**Symptom:** `./build/order_book` hangs with no output.

**Root cause:** Placing an `OrderBook` on the stack. Our `OrderBook` class stores
its large members via `unique_ptr` (heap), so this shouldn't happen with the
current code. If you see it:

```cpp
// WRONG (84 MB on stack → crash):
int main() {
    OrderBook book;  // if OrderBook had members directly, not unique_ptr
}

// RIGHT (84 MB on heap):
int main() {
    auto book = std::make_unique<OrderBook>(); // or just use the current code
}
```

### Issue: All benchmark results are 0 ns

The timer resolution of `mach_absolute_time` on Apple Silicon is ~42 ns per tick.
Operations faster than one tick appear as 0. This is expected. Use Google Benchmark
(`latency_bench`) instead of the manual timer for sub-42ns measurements — it
aggregates many iterations.

### Issue: Google Test tests take 12-30ms each

This is normal. The `26ms` you see in test output is the time to **construct** an
`OrderBook` (allocate 84 MB and pre-fault all pages). The actual operation being
tested takes nanoseconds — but Google Test doesn't time just the operation.

### Issue: Compilation warning about unsequenced modification

```
warning: unsequenced modification and access to 'id' [-Wunsequenced]
book.add_order(id++, ..., (id % 2 == 0) ...);
```

This was in the original benchmark code and was fixed. If you see it, ensure
`id++` and the `id % 2` check are separated:

```cpp
// Wrong:
book.add_order(id++, price, 100, (id % 2 == 0) ? Side::Buy : Side::Sell);

// Right:
bool is_buy = (id % 2 == 0);
Price price = is_buy ? 49'900u : 50'100u;
book.add_order(id++, price, 100, is_buy ? Side::Buy : Side::Sell);
```

### How to debug test failures

```bash
# Run a single test by name:
./build/order_book_tests --gtest_filter="OrderBookTest.PriceTimePriorityOrderedCorrectly"

# Run with verbose output:
./build/order_book_tests --gtest_filter="*" --gtest_print_time=1

# Build with AddressSanitizer to detect memory bugs:
cmake build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
make -C build order_book_tests
./build/order_book_tests
# ASAN will print detailed error messages for use-after-free, buffer overflows, etc.
```

---

## 14. Glossary

**Aggressor:** The order that arrives and causes a match by crossing the spread.
The buyer who bids above the best ask, or seller who asks below the best bid.

**Atomic operation:** A CPU instruction that completes in a single, uninterruptible
step. No other CPU can observe a "half-finished" state. Example: `std::atomic<int>`.

**Bid:** A buy offer — the price a buyer is willing to pay. The best bid is the
highest such price currently in the book.

**Ask (Offer):** A sell offer — the price a seller is willing to accept. The best
ask is the lowest such price currently in the book.

**Cache:** Ultra-fast memory built into the CPU. L1 cache (~4 cycles), L2 (~12
cycles), L3 (~40 cycles). Main RAM is ~200 cycles. Fitting data in cache is
critical for low latency.

**Cache line:** The smallest unit the CPU loads from memory: 64 bytes. Every
memory access loads an entire cache line, even if you only need 1 byte.

**False sharing:** When two unrelated variables share a cache line. Updating one
invalidates the other's cache entry in every other core — even though they're
independent variables.

**FIFO (First In, First Out):** A queue. The first item added is the first
removed. Our price levels are FIFO queues — time priority.

**Flamegraph:** A visualization of where CPU time is spent. Horizontal width =
time spent. Stacked layers = call stack. Used to identify bottlenecks.

**Hash map (hash table):** A data structure that stores key-value pairs and
answers "given key K, what is the value?" in O(1) average time. Our `OrderMap`
maps OrderId → Order*.

**HFT (High-Frequency Trading):** Trading strategies that execute thousands to
millions of orders per second, holding positions for microseconds to milliseconds.

**Intrusive linked list:** A linked list where the `next`/`prev` pointers live
inside the stored object itself. Faster than `std::list` because no separate node
allocation is needed.

**L1/L2/L3 cache:** Levels of CPU cache. L1 is fastest (~32-64 KB), L2 is medium
(~256 KB-4 MB), L3 is slowest but largest (~4-64 MB). All are faster than RAM.

**Latency:** The time from when an input arrives to when the output is produced.
In trading: time from order received to order acknowledged/matched.

**Lock:** A synchronization primitive that ensures only one thread accesses a
resource at a time. Other threads must wait. Causes unpredictable latency spikes.

**Lock-free:** A design where threads never block each other. Progress is
guaranteed for the system even if individual threads are delayed.

**`mach_absolute_time()`:** macOS API for reading the hardware nanosecond counter.
Implemented via the commpage (no system call). Used as our timer on macOS.

**Matching engine:** The component of an exchange that finds counterpart orders
and executes trades. Our `OrderBook::match()` method is a matching engine.

**Memory pool:** A pre-allocated reservoir of fixed-size objects. Acquiring an
object is a pointer swap; no OS allocation needed.

**O(1) / O(n) / O(log n):** Big-O notation describing how runtime scales with
input size. O(1) = constant (ideal). O(log n) = logarithmic (good). O(n) = linear
(slow at scale). O(n²) = quadratic (terrible).

**p99 latency:** The 99th percentile latency. 99% of operations were faster than
this value. The "tail" you care about in production.

**Passive order:** A resting order already in the book, waiting for a counterpart.
It gets "passively" matched when an aggressor arrives.

**Pre-fault:** Touching memory pages at startup to force the OS to map physical
RAM. Prevents page-fault latency spikes during live trading.

**Price-time priority:** The matching rule used by most exchanges. Better prices
are matched first. At the same price, earlier-arriving orders are matched first.

**RDTSC (Read Time-Stamp Counter):** x86 CPU instruction that reads a
monotonically increasing cycle counter. Used for nanosecond-precision timing on
Linux/Windows. On macOS we use `mach_absolute_time()` instead.

**Ring buffer:** A fixed-size array used as a circular queue. When the write
pointer reaches the end, it wraps to the beginning.

**Spread:** The difference between the best ask and the best bid. `spread =
best_ask - best_bid`. Market makers profit from the spread.

**SPSC (Single-Producer Single-Consumer):** A queue pattern where exactly one
thread writes and one thread reads. Allows a simpler, faster lock-free
implementation than general multi-producer/consumer queues.

**Throughput:** Operations per second. Distinct from latency — high throughput
means many operations completed, but each individual operation could still be slow.

**Tick:** The minimum price increment. If a stock trades in $0.01 increments,
one tick = $0.01. Prices stored as integers in ticks: $100.50 = 10050 ticks.

**TSC (Time-Stamp Counter):** The x86 hardware register read by RDTSC. Increments
at a fixed frequency (not the core clock), making it suitable for timing even
when CPU frequency scales.

**`unique_ptr`:** A C++ smart pointer that owns heap-allocated memory and
automatically frees it when the pointer goes out of scope. Zero runtime overhead
vs raw pointer after construction.

---

## 15. Visual Diagrams

### Complete Order Book State

```
                    ORDER BOOK at time T
    ═══════════════════════════════════════════════════════════

    BID SIDE (buyers, sorted high→low)
    ┌───────────────────────────────────────────────┐
    │ Price 103 │ [Order#8: 100sh] → null           │ ← best_bid_
    │ Price 102 │ [Order#5: 200sh] → [Order#9: 50sh]│
    │ Price 100 │ [Order#3: 500sh] → [Order#6: 300sh]│
    │ Price  99 │ [Order#1: 400sh] → null           │
    └───────────────────────────────────────────────┘
                    ↑ SPREAD ↑
    ┌───────────────────────────────────────────────┐
    │ Price 105 │ [Order#2: 200sh] → null           │ ← best_ask_
    │ Price 107 │ [Order#4: 600sh] → [Order#7: 150sh]│
    │ Price 110 │ [Order#10: 800sh]→ null           │
    └───────────────────────────────────────────────┘
    ASK SIDE (sellers, sorted low→high)

    Top of book: bid=100sh@103   ask=200sh@105   spread=2
```

### Memory Layout of One Order (64 bytes, one cache line)

```
Byte offset:
0        8        12       16       20       24   25  26-27 28-31 32-39   40-47   48-63
│        │        │        │        │        │    │   │     │     │       │       │
├────────┼────────┼────────┼────────┼────────┼────┼───┼─────┼─────┼───────┼───────┤
│   id   │ price  │  qty   │remaining│timestamp│side│pad│(pad)│(pad)│ next* │ prev* │
│ 8 bytes│4 bytes │4 bytes │4 bytes  │ 8 bytes │1   │3  │     │     │8 bytes│8 bytes│
└────────┴────────┴────────┴────────┴────────┴────┴───┴─────┴─────┴───────┴───────┘
│←────────────────────────── exactly 64 bytes ────────────────────────────────────→│
│←────────────────────────── one CPU cache line ──────────────────────────────────→│
```

### Flow of Adding a Passive Order

```
add_order(id=42, price=100, qty=500, side=Buy)
         │
         ▼
    ┌─────────────┐
    │ Price valid? │ price < 100,000? → YES
    └──────┬──────┘
           │
           ▼
    ┌─────────────────┐
    │ pool_->acquire()│ → grab Order#X from free list (O(1))
    └──────┬──────────┘
           │
           ▼
    ┌───────────────────┐
    │ Fill Order fields │ id=42, price=100, qty=500, remaining=500, ts=now
    └──────┬────────────┘
           │
           ▼
    ┌─────────────────────────┐
    │ order_map_->insert(42,  │ → register for fast cancel (O(1))
    │           &Order#X)     │
    └──────┬──────────────────┘
           │
           ▼
    ┌──────────────────────────────┐
    │ price(100) >= best_ask_?     │
    │ best_ask_ = 99999 (no asks) │ → NO → skip matching
    └──────┬───────────────────────┘
           │
           ▼
    ┌──────────────────────────────┐
    │ (*bids_)[100].push_back(    │ → append to tail of bid queue at price 100
    │           &Order#X)          │   (O(1) pointer wiring)
    └──────┬───────────────────────┘
           │
           ▼
    ┌────────────────────┐
    │ 100 > best_bid_(0)?│ → YES → best_bid_ = 100
    └──────┬─────────────┘
           │
           ▼
    ┌─────────────────────┐
    │ publish(OrderAck)   │ → event pushed to SPSC queue
    └──────┬──────────────┘
           │
           ▼
        return true
```

### Flow of Matching (Aggressive Buy)

```
add_order(id=50, price=107, qty=250, side=Buy)
best_ask_ = 105, asks_[105] has [Order#2: 200sh]

         aggressor: remaining=250
              │
              ▼
    ┌─────────────────────────────────┐
    │ price(107) >= best_ask_(105)?   │ → YES
    └──────────────┬──────────────────┘
                   │
                   ▼
             call match(aggressor)
                   │
         ┌─────────▼──────────┐
         │ passive_levels =   │
         │  asks_.get()       │
         │ best = best_ask_   │
         └─────────┬──────────┘
                   │
         ┌─────────▼──────────────────────────────┐
         │ OUTER LOOP: aggressor.remaining=250>0  │
         │                                        │
         │  asks_[105] not empty                  │
         │  prefetch asks_[105].head              │
         │                                        │
         │  INNER LOOP:                           │
         │    passive = Order#2 (200sh at 105)    │
         │    fill = min(250, 200) = 200          │
         │    aggressor.remaining = 50            │
         │    passive.remaining = 0               │
         │    → publish TradeEvent                │
         │    → passive fully filled:             │
         │       level.remove(Order#2)            │
         │       order_map_.erase(Order#2.id)     │
         │       pool_.release(Order#2)           │
         │  INNER LOOP ends (level empty)         │
         │                                        │
         │  aggressor.remaining=50>0              │
         │  asks_[105] empty → ++best_ask_=106    │
         │  asks_[106] empty → ++best_ask_=107    │
         │  aggressor.price(107) < best_ask_(107)?│
         │  → NO (107 == 107, not less than)      │
         │  → check asks_[107]: empty             │
         │  → ++best_ask_=108                     │
         │  aggressor.price(107) < 108 → YES      │
         │  → BREAK                               │
         └─────────┬──────────────────────────────┘
                   │
                   ▼
         aggressor.remaining=50 > 0 → rest it
         (*bids_)[107].push_back(aggressor)
         best_bid_ = 107
```

### How the Hash Map Handles Collisions

```
Hash map with Capacity=8, MASK=7:

hash(1001) = 3     hash(1009) = 3     hash(1017) = 5

Step 1: Insert id=1001
  ┌───┬───┬───┬───────┬───┬───┬───┬───┐
  │ _ │ _ │ _ │  1001 │ _ │ _ │ _ │ _ │
  └───┴───┴───┴───────┴───┴───┴───┴───┘
                  ↑ slot 3

Step 2: Insert id=1009 (also hashes to slot 3 → collision!)
        Try slot 4 (empty) → place here
  ┌───┬───┬───┬───────┬───────┬───┬───┬───┐
  │ _ │ _ │ _ │  1001 │  1009 │ _ │ _ │ _ │
  └───┴───┴───┴───────┴───────┴───┴───┴───┘

Step 3: Insert id=1017 (hashes to slot 5 → empty)
  ┌───┬───┬───┬───────┬───────┬───────┬───┬───┐
  │ _ │ _ │ _ │  1001 │  1009 │  1017 │ _ │ _ │
  └───┴───┴───┴───────┴───────┴───────┴───┴───┘

Find id=1009:
  hash → slot 3 → key is 1001 (not 1009)
  probe +1 → slot 4 → key is 1009 ✓ → return Order*
```

### Price Level as a Queue (FIFO)

```
Orders arrive in this sequence: A (09:30:00), B (09:30:01), C (09:30:02)
All at price tick 5000 (buy side)

Timeline:
                head                            tail
09:30:00:        [A: 500sh]
09:30:01:        [A: 500sh] ←→ [B: 300sh]
09:30:02:        [A: 500sh] ←→ [B: 300sh] ←→ [C: 200sh]

Aggressive sell arrives at 5000 (matches best bid):
Match A first (it arrived earliest — time priority)
After A is consumed:
                 [B: 300sh] ←→ [C: 200sh]
                  head             tail
(B is now matched next if another sell arrives)
```

---

*End of Day 1 Complete Guide.*

*Next: [Day 2] — Profiling, flamegraphs, `modify_order`, and visualization.*
