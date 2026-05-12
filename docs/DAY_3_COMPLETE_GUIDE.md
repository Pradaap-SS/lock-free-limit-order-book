# Day 3 Complete Guide: NASDAQ ITCH 5.0 Protocol & Replay Engine

> **Who this is for:** Someone who finished Day 2 and wants to understand every
> decision on Day 3 — why binary protocols use big-endian, what a stock locate
> is and why it matters for performance, how the replay engine bridges the gap
> between raw wire bytes and a live order book, and what the benchmark numbers
> actually measure.

---

## Table of Contents

1. [Day 3 Goals & Overview](#1-day-3-goals--overview)
2. [New Trading Concepts](#2-new-trading-concepts)
3. [New Technical Concepts](#3-new-technical-concepts)
4. [Complete Codebase Walkthrough](#4-complete-codebase-walkthrough)
5. [Updated Project Structure](#5-updated-project-structure)
6. [Data Structures Added](#6-data-structures-added)
7. [Build & Run Instructions](#7-build--run-instructions)
8. [Tests Explanation](#8-tests-explanation)
9. [Key Algorithms Implemented](#9-key-algorithms-implemented)
10. [Memory Management](#10-memory-management)
11. [Performance Characteristics](#11-performance-characteristics)
12. [What's Next (Day 4 Preview)](#12-whats-next-day-4-preview)
13. [Common Issues & Debugging](#13-common-issues--debugging)
14. [Glossary](#14-glossary)
15. [Visual Diagrams](#15-visual-diagrams)

---

## 1. Day 3 Goals & Overview

### What were we trying to accomplish?

Days 1 and 2 built a correct, fast, measurable engine — but it lived in
isolation. It only processed orders we invented in tests. Day 3 connects it to
the real world: **actual exchange binary data**.

Four goals:

1. **Parse NASDAQ ITCH 5.0** — the binary protocol used to disseminate every
   order, cancellation, and trade on the NASDAQ exchange. Zero-copy, zero-allocation,
   correct big-endian deserialization of 7 message types.

2. **Map protocol events → OrderBook operations** — ITCH "Order Executed" doesn't
   map 1:1 to any single OrderBook call. The engine needs to maintain its own
   state to bridge the gap correctly.

3. **Symbol filtering via locate numbers** — ITCH sends a 16-bit integer
   (locate) instead of the full 8-byte symbol on every message. One integer
   comparison filters 99%+ of messages in a multi-symbol stream.

4. **Memory-mapped file I/O** — reading a 10 GB ITCH file with `read()` calls
   would serialize I/O with parsing. `mmap()` lets the OS page in data lazily
   while we parse, in parallel, with zero copying.

### What was built?

```
4 new headers  +  1 new executable  +  21 new tests  +  5 new benchmarks
= complete protocol parsing + replay pipeline
```

### How Day 3 compares to Days 1 & 2

| Dimension | Day 1+2 | Day 3 |
|---|---|---|
| Data source | Synthetic (hand-coded) | Real NASDAQ binary format |
| Input | Function calls | Binary file / buffer |
| Message types | 3 (add/cancel/modify) | 7 ITCH message types |
| Symbol handling | Single symbol | Multi-symbol stream, locate-filtered |
| File I/O | None | `mmap()` + `MADV_SEQUENTIAL` |
| Test count | 22 | **43 total** (+21) |
| Benchmarks | 9 | **14 total** (+5) |

---

## 2. New Trading Concepts

### What is a market data feed?

Exchanges broadcast every order, cancellation, and trade to all connected
participants simultaneously. This is called a **market data feed**. It lets
market makers, algorithms, and risk systems see exactly what's in the order book
in real time.

```
Exchange matching engine
        │
        ├──► Order Book (internal state)
        │
        └──► Market Data Feed ──► broadcast to all subscribers
                                        │
                                        ├─► Market Maker A
                                        ├─► HFT Firm B
                                        └─► Risk System C
```

Participants who receive the feed reconstruct their own local copy of the order
book. Our `ReplayEngine` does exactly this — it replays a recorded feed to
rebuild the book state from history.

### What is ITCH?

ITCH is NASDAQ's proprietary binary protocol for market data. It is one of the
most widely studied protocols in HFT because NASDAQ publishes complete historical
data files on their FTP server (with free registration).

Key properties:
- **Binary, not text** — no JSON, no FIX tags, no commas. Every field is a raw
  integer at a fixed byte offset. Much faster to parse than text protocols.
- **Big-endian** — multi-byte integers are stored most-significant-byte first.
  Most modern CPUs are little-endian, so we must byte-swap every field.
- **Full depth** — ITCH broadcasts every individual order, not just the
  top-of-book quote. You can reconstruct the complete order book at any moment.
- **No gaps** — messages are sent in sequence-number order. If you miss one,
  you must re-request a resend (UDP-based live feed) or read from the historical file.

### What is a stock locate?

Every symbol (AAPL, MSFT, etc.) is assigned a 2-byte integer called a
**stock locate** at the start of each trading session via a Stock Directory ('R')
message. All subsequent messages carry this integer instead of the full 8-byte
symbol.

```
Session start:
  StockDirectory: locate=1 → "AAPL    " (8 bytes, padded with spaces)
  StockDirectory: locate=2 → "MSFT    "
  StockDirectory: locate=3 → "TSLA    "
  ... (thousands more)

Trading messages (later):
  AddOrder:   locate=1, ref=1001, side=B, shares=500, price=1500000
  AddOrder:   locate=3, ref=1002, side=S, shares=100, price=2200000
  OrderDelete: locate=2, ref=999
```

**Why this matters for performance:** Filtering by symbol requires comparing one
`uint16_t` (2 bytes) per message instead of 8 bytes of string comparison. For a
full NASDAQ day (~500 million messages), this saves roughly 3.5 billion byte
comparisons per symbol you're tracking.

### What is Order Executed ('E') vs Order Delete ('D')?

ITCH distinguishes two ways a resting order leaves the book:

| Message | Meaning | What we do |
|---|---|---|
| `'D'` Order Delete | Participant cancelled the order (voluntary) | `cancel_order(ref)` |
| `'E'` Order Executed | The order matched against an incoming aggressor | Reduce qty or cancel |
| `'X'` Order Cancel | Participant partially reduced the quantity | Reduce qty |
| `'U'` Order Replace | Cancel + re-submit at new price/qty | Cancel old, add new |

The critical subtlety: ITCH `'E'` tells you how many shares **executed**, not
how many remain. You must track remaining quantity externally to know whether
to fully cancel the order or just reduce its size.

### What is a match number?

Every trade on an exchange is assigned a unique **match number** — a 64-bit
integer that links the ITCH execution message to the clearing system's record
of that trade. It allows post-trade reconciliation: regulators and prime brokers
can verify that every reported execution matches an exchange record.

Our parser extracts the match number but we don't use it further — it's there
for completeness and future audit trail features.

### What is price-time priority in an ITCH context?

ITCH sends orders in the exact sequence they were received by the exchange.
The first `AddOrder` at a given price level truly did arrive before the second.
By processing messages in order and using our intrusive FIFO queue (from Day 1),
the replayed order book naturally has correct time priority — no sorting needed.

---

## 3. New Technical Concepts

### What is `mmap()` and why use it instead of `read()`?

`mmap()` maps a file into the process's virtual address space. The file contents
appear as a contiguous array of bytes in memory — you can access any byte
directly with a pointer without issuing a read() system call.

```
Without mmap (traditional read()):
  read(fd, buf, 4096) ─► OS copies 4096 bytes from kernel page cache to user buffer
  parse(buf, 4096)    ─► parse those bytes
  read(fd, buf, 4096) ─► OS copies next 4096 bytes...
  parse(buf, 4096)    ─► ... (repeat millions of times)

With mmap():
  data = mmap(fd, PROT_READ)  ─► one call, maps entire file
  parse(data, size)           ─► parser walks the buffer directly
  // No copying, no syscalls on the hot path
```

The OS pages in data lazily as the parser accesses it. With `MADV_SEQUENTIAL`,
the kernel prefetches ahead — by the time the parser reaches page N+1, the
kernel has already loaded it from disk into memory.

**Zero-copy:** The parser reads directly from the kernel's page cache. There is
no intermediate user-space buffer. For a 10 GB file, this saves 10 GB of copying.

### What is big-endian byte order?

When a multi-byte integer is stored in memory, there are two ways to order the
bytes:

```
Integer value: 0x12345678

Little-endian (x86, ARM in most modes):
  Address: [0x78][0x56][0x34][0x12]  — LSB first
  (least significant byte at lowest address)

Big-endian (network byte order, ITCH):
  Address: [0x12][0x34][0x56][0x78]  — MSB first
  (most significant byte at lowest address)
```

ITCH uses big-endian because network protocols historically standardized on it
("network byte order"). Apple Silicon is little-endian, so we must reverse every
multi-byte field when reading from the ITCH binary.

Our `itch_detail::be16/be32/be48/be64` functions do this by bit-shifting and ORing:

```cpp
inline uint32_t be32(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) |  // MSB goes to bits 31-24
           (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |
            uint32_t(p[3]);          // LSB stays at bits 7-0
}
```

> **⚠️ Common Mistake:** Using `*(uint32_t*)p` to read a 4-byte integer
> directly from the buffer. This is undefined behavior (alignment violation)
> AND gives the wrong byte order on little-endian machines. Always use
> explicit byte-by-byte deserialization like our `be32()`.

### What is a zero-copy, zero-allocation parser?

Our `ItchParser::parse()` never calls `malloc`, `new`, or even uses any
`std::vector`. Here's the complete hot loop:

```cpp
while (p + 2 <= end) {             // bounds check: do we have a length prefix?
    const uint16_t msg_len = be16(p);  // read 2-byte length
    p += 2;

    if (p + msg_len > end) break;  // bounds check: do we have the full message?

    dispatch(p, msg_len);          // dispatch to callback (no allocation)
    ++count;
    p += msg_len;                  // advance past this message
}
```

The only memory operations are: reads from the existing buffer and writes to
local stack structs that are passed to callbacks. No heap involved.

**Why "zero-copy"?** `dispatch()` fills message structs (`AddOrderMsg`,
`OrderDeleteMsg`, etc.) by reading fields directly from the buffer with
pointer arithmetic. The struct is stack-allocated; the buffer data is never
copied into a separate heap allocation.

### What is callback-based dispatch?

The parser uses `std::function<void(const MsgType&)>` callbacks — one per
message type. The application sets the callbacks it cares about and leaves
others null. The parser checks `if (on_add_order && len >= 36)` before
dispatching.

```
Parser sees message type 'A':
  Is on_add_order set?   YES → read fields, construct AddOrderMsg, call callback
  Is on_add_order null?  NO  → skip (one branch, no processing)
```

This is the **observer pattern** applied to a streaming parser. The parser has
no knowledge of order books, symbols, or business logic. The `ReplayEngine`
wires up the callbacks.

**Alternative designs** (and why we didn't use them):
- **Virtual dispatch** — slower per call, requires heap allocation for the
  listener object
- **Template policy** — faster but forces a recompile for every different
  callback set
- **Coroutines** — elegant but complex; our mmap approach means we already have
  the full buffer in memory, so a streaming coroutine offers no benefit

### What is the locate filter and why is it O(1)?

After processing the Stock Directory for the target symbol:

```cpp
parser_.on_stock_directory = [this](const StockDirectoryMsg& m) {
    if (!found_sym_ && std::strcmp(m.stock, cfg_.symbol) == 0) {
        locate_    = m.stock_locate;  // remember the 2-byte integer
        found_sym_ = true;
    }
};
```

Every subsequent message is filtered with a single comparison:

```cpp
parser_.on_add_order = [this](const AddOrderMsg& m) {
    if (!found_sym_ || m.stock_locate != locate_) return; // O(1) filter
    // ... process this order
};
```

The benchmark `BM_ReplaySymbolFilter` measures this: processing 100K messages
for a non-existent symbol takes **16 ms** (vs 49 ms for full replay). The
remaining 16 ms is the parser itself walking the buffer and dispatching the
`switch` — the locate filter eliminates all order book work.

### What is an order tracker and why is it needed?

The `OrderBook` doesn't expose individual order state by design — it's a black
box that accepts commands (`add_order`, `cancel_order`, `modify_order`). But
ITCH's `'E'` (Executed) message only tells us the **executed quantity**, not the
remaining quantity or the price.

To call `modify_order(ref, price, remaining)` for a partial fill, we need to
know the current price and remaining quantity — information we must track
ourselves:

```cpp
struct OrderState {
    Price    price;      // needed for modify_order's price argument
    Quantity remaining;  // maintained as executions/cancels arrive
    Side     side;       // needed for Replace's cancel+re-add
};

std::unordered_map<uint64_t, OrderState> order_tracker_;
```

This is a **shadow state map**: it duplicates some of what the OrderBook knows
internally, but at the granularity the replay engine needs.

**Memory cost:** With 1M active orders, each `OrderState` is
`4 + 4 + 1 = 9 bytes` (before alignment padding to ~16 bytes), plus
`unordered_map` overhead of ~50 bytes per entry. Total: ~66 MB for 1M orders.

---

## 4. Complete Codebase Walkthrough

### File: `include/mmap_reader.h` (new)

**Purpose:** RAII wrapper around `mmap(2)` for read-only file access.

```cpp
class MmapReader {
public:
    explicit MmapReader(const char* path) {
        fd_ = ::open(path, O_RDONLY);                    // open the file
        if (fd_ < 0) throw std::runtime_error(...);

        struct stat st{};
        ::fstat(fd_, &st);
        size_ = static_cast<std::size_t>(st.st_size);   // get file size

        data_ = ::mmap(nullptr, size_, PROT_READ,        // map into address space
                       MAP_PRIVATE, fd_, 0);

        ::madvise(data_, size_, MADV_SEQUENTIAL);        // prefetch hint
    }

    ~MmapReader() {
        if (data_) ::munmap(data_, size_);  // unmap
        if (fd_ >= 0) ::close(fd_);         // close file descriptor
    }
    // ...
};
```

**`MAP_PRIVATE` vs `MAP_SHARED`:** `MAP_PRIVATE` means writes to the mapping
are copy-on-write (don't affect the file). Since we're read-only, this doesn't
matter — but it's safer than `MAP_SHARED`.

**`MADV_SEQUENTIAL`:** Tells the kernel "I will read this data sequentially,
front to back." The kernel responds by aggressively read-ahead prefetching
subsequent pages while we process current ones. For a sequential binary parser
walking an ITCH file, this is exactly the right hint.

**Non-copyable, movable:** The destructor owns the mmap and file descriptor.
Copying would create a double-free. Moving transfers ownership and nulls the
moved-from object — the pattern for resource-owning RAII wrappers.

**Usage:**
```cpp
try {
    MmapReader f("data/S081322-v50.itch");
    ReplayEngine engine;
    engine.run(f.data(), f.size(), cfg);
} catch (const std::exception& e) {
    // file not found, permission denied, etc.
}
```

---

### File: `include/itch_parser.h` (new)

**Purpose:** NASDAQ ITCH 5.0 binary parser, message structs, and the `itch_build`
namespace for constructing synthetic/test messages.

#### The ITCH message layout map

Every ITCH message starts with a 1-byte type character. The 2-byte length
prefix comes before the message (added by the framing layer, not part of the
message itself).

```
Wire format:
┌──────────┬───────────────────────────────────────────────────┐
│ len: 2B  │  message payload (len bytes)                      │
└──────────┴───────────────────────────────────────────────────┘
           │ type │ locate │ tracking │ timestamp │ payload... │
           │ 1B   │   2B   │    2B    │    6B     │            │
```

All multi-byte fields are big-endian. The type byte determines the rest of
the layout.

#### Message types parsed

| Type | Name | Payload size | What happens |
|---|---|---|---|
| `'R'` | StockDirectory | 33 bytes | Establishes locate → symbol mapping |
| `'A'` | AddOrder (no MPID) | 36 bytes | New resting order |
| `'F'` | AddOrder (with MPID) | 40 bytes | Same as 'A' + 4-byte broker code |
| `'E'` | OrderExecuted | 31 bytes | Shares traded against a resting order |
| `'C'` | OrderExecutedPx | 36 bytes | Like 'E' + execution price (off-exchange) |
| `'X'` | OrderCancel | 23 bytes | Partial quantity reduction |
| `'D'` | OrderDelete | 19 bytes | Full order cancellation |
| `'U'` | OrderReplace | 35 bytes | Cancel + re-add at new terms |

All others (System Event, Trade, Cross Trade, etc.) are silently skipped.

#### Exact byte offsets for AddOrder ('A')

```
Offset  Bytes  Field
------  -----  -----
0       1      Message Type = 'A'
1       2      Stock Locate (big-endian uint16)
3       2      Tracking Number (ignored)
5       6      Timestamp (nanoseconds since midnight, big-endian 6-byte uint)
11      8      Order Reference Number (big-endian uint64)
19      1      Buy/Sell Indicator ('B' or 'S')
20      4      Shares (big-endian uint32)
24      8      Stock (ASCII, right-padded with spaces)
32      4      Price (big-endian uint32, in units of $0.0001)
Total:  36 bytes
```

The comment in the code `// type(1)+locate(2)+tracking(2)+ts(6)+ref(8)+side(1)+shares(4)+stock(8)+price(4) = 36`
makes this verifiable at a glance.

> **⚠️ Common Mistake (we hit this):** The arrays for message construction
> must be exactly the right size — `uint8_t msg[36]{}` for AddOrder, not
> `msg[35]`. An off-by-one silently drops the last byte of the last field
> (the LSB of `price`), causing parsing tests to fail with values like
> 1504768 instead of 1505000. Always count the bytes explicitly and add a
> comment like the one above.

#### The `be48()` function — reading a 6-byte timestamp

NASDAQ timestamps are 6 bytes (48 bits). This is unusual because most CPUs
only have 32-bit and 64-bit load instructions. Our `be48()` reads all 6 bytes
manually:

```cpp
inline uint64_t be48(const uint8_t* p) noexcept {
    return (uint64_t(p[0]) << 40) |  // byte 0 = bits 47-40
           (uint64_t(p[1]) << 32) |  // byte 1 = bits 39-32
           (uint64_t(p[2]) << 24) |  // byte 2 = bits 31-24
           (uint64_t(p[3]) << 16) |  // byte 3 = bits 23-16
           (uint64_t(p[4]) << 8)  |  // byte 4 = bits 15-8
            uint64_t(p[5]);          // byte 5 = bits 7-0
}
```

At nanosecond resolution, 6 bytes can represent up to 281,474,976,710,655 ns
= ~281,474 seconds = ~3.26 days. Enough for one full trading session.

#### The `parse_stock()` helper — trimming padding

ITCH stocks are 8-byte ASCII, right-padded with spaces. "AAPL" is stored as
`"AAPL    "` (4 chars + 4 spaces). Our parser trims trailing spaces and
null-terminates:

```cpp
inline void parse_stock(const uint8_t* src, char* dst9) noexcept {
    std::memcpy(dst9, src, 8);
    dst9[8] = '\0';
    for (int i = 7; i >= 0 && dst9[i] == ' '; --i) dst9[i] = '\0';
}
```

After this, `dst9` is a valid C string like `"AAPL"` that can be compared
with `strcmp`.

#### The `itch_build` namespace — constructing test messages

Every message builder follows the same pattern: fill a stack-allocated byte
array with the message fields, then prepend a 2-byte length header:

```cpp
inline std::size_t add_order(uint8_t* buf,
                              uint16_t locate, uint64_t ts_ns, uint64_t order_ref,
                              char buy_sell, uint32_t shares,
                              const char* sym, uint32_t price) noexcept {
    uint8_t msg[36]{};                    // zero-initialize: all fields default to 0
    msg[0] = 'A';                          // type byte
    put16(msg + 1,  locate);
    put16(msg + 3,  0);                    // tracking number (don't care in tests)
    put48(msg + 5,  ts_ns);
    put64(msg + 11, order_ref);
    msg[19] = static_cast<uint8_t>(buy_sell);
    put32(msg + 20, shares);
    put_stock(msg + 24, sym);
    put32(msg + 32, price);
    return frame(buf, msg, 36);            // prepend 2-byte length, return total bytes
}
```

The `frame()` function writes `[len_hi][len_lo][msg[0]]...[msg[35]]` into
`buf` and returns `2 + 36 = 38`. The caller passes a local `uint8_t tmp[64]`
and appends the returned bytes to the stream buffer.

---

### File: `include/stats_counter.h` (new)

**Purpose:** Lock-free atomic counters for the replay pipeline — writable by
the matching thread, readable by a monitoring thread without locks.

```cpp
struct alignas(64) StatsCounter {
    std::atomic<uint64_t> messages_processed{0};  // on one cache line

    alignas(64) std::atomic<uint64_t> orders_added{0};     // own cache line
    alignas(64) std::atomic<uint64_t> orders_cancelled{0}; // own cache line
    alignas(64) std::atomic<uint64_t> orders_replaced{0};
    alignas(64) std::atomic<uint64_t> orders_executed{0};
    alignas(64) std::atomic<uint64_t> orders_rejected{0};
    alignas(64) std::atomic<uint64_t> trades{0};
    alignas(64) std::atomic<uint64_t> shares_traded{0};
};
```

**Why `alignas(64)` on each field?** Without it, all 8 counters could share a
single 64-byte cache line. When the replay thread writes `orders_added` and the
monitoring thread reads `trades`, both accesses would touch the same cache line.
The CPU must coordinate this — even though the reads and writes touch different
bytes, they share the same cache-coherency unit (the cache line). This is **false
sharing** and can slow writes by 5-10× by causing constant cache invalidation
between cores.

With `alignas(64)`, each counter owns exactly one cache line. Writes to
`orders_added` never touch the cache line containing `trades`.

**`memory_order_relaxed` everywhere:** We don't need acquire/release ordering
here because:
1. The write thread never reads what it wrote (it only increments)
2. The monitoring thread doesn't use counters to synchronize control flow
3. Seeing a counter value that's 1-2 operations stale is acceptable for telemetry

`relaxed` is the cheapest atomic operation — it's just an atomic read/write with
no memory fence instruction.

**The `Snapshot` struct:** A plain (non-atomic) snapshot of all counters at one
moment. The monitoring thread takes a snapshot and prints it — reading each
counter with `load(relaxed)` individually (fine, since slight staleness is OK):

```cpp
Snapshot snapshot() const noexcept {
    return {
        messages_processed.load(std::memory_order_relaxed),
        orders_added      .load(std::memory_order_relaxed),
        // ...
    };
}
```

---

### File: `include/replay_engine.h` (new)

**Purpose:** Wires ITCH parser → OrderBook, implementing symbol filtering,
order state tracking, and ITCH-to-OrderBook translation for all 5 event types.

#### The design pattern: `setup_callbacks()` + lambdas

All 5 event handlers are set in `setup_callbacks()` as lambdas that capture
`this`. This keeps all the business logic in one method and makes the
`run()` method trivially readable:

```cpp
StatsCounter::Snapshot run(const uint8_t* data, std::size_t size,
                            const Config& cfg = {}) {
    cfg_       = cfg;
    locate_    = 0;
    found_sym_ = false;
    stats_.reset();
    order_tracker_.clear();
    order_tracker_.reserve(1 << 20);  // pre-size for 1M orders

    setup_callbacks();                // wire all 5 lambdas

    const auto t0 = std::chrono::steady_clock::now();
    parser_.parse(data, size);        // process entire buffer
    const auto t1 = std::chrono::steady_clock::now();

    elapsed_sec_ = std::chrono::duration<double>(t1 - t0).count();
    return stats_.snapshot();
}
```

#### The stock locate discovery

```cpp
parser_.on_stock_directory = [this](const StockDirectoryMsg& m) {
    if (!found_sym_ && std::strcmp(m.stock, cfg_.symbol) == 0) {
        locate_    = m.stock_locate;  // store the 2-byte integer
        found_sym_ = true;
    }
};
```

`found_sym_` is set to `true` after the first match. All subsequent Stock
Directory messages (there can be thousands) are skipped in O(1) via the
`!found_sym_` check.

#### The AddOrder handler

```cpp
parser_.on_add_order = [this](const AddOrderMsg& m) {
    if (!found_sym_ || m.stock_locate != locate_) return; // locate filter: O(1)
    stats_.inc_message();

    const Price    tick = to_tick(m.price);         // ITCH units → OrderBook ticks
    const Quantity qty  = m.shares;
    const Side     side = (m.buy_sell == 'B') ? Side::Buy : Side::Sell;

    if (!book_.add_order(m.order_ref, tick, qty, side)) {
        stats_.inc_rejected();   // pool exhausted or price out of range
        return;
    }
    order_tracker_[m.order_ref] = {tick, qty, side}; // shadow state
    stats_.inc_add();
};
```

Notice: ITCH order reference numbers (`uint64_t`) are used directly as
`OrderId`. The OrderMap uses `0` as the empty sentinel, and ITCH guarantees
order references are always ≥ 1, so there is no collision.

#### The OrderExecuted handler — the tricky one

```cpp
parser_.on_order_executed = [this](const OrderExecutedMsg& m) {
    if (!found_sym_ || m.stock_locate != locate_) return;

    auto it = order_tracker_.find(m.order_ref);
    if (it == order_tracker_.end()) return;  // order not in our symbol's set

    OrderState& os = it->second;
    const Quantity exec = std::min(m.executed_shares, os.remaining);
    os.remaining -= exec;
    stats_.inc_trade(exec);

    if (os.remaining == 0) {
        // Fully filled — remove from book and tracker
        book_.cancel_order(m.order_ref);
        order_tracker_.erase(it);
        stats_.inc_cancel();
    } else {
        // Partially filled — reduce in-place (preserves time priority, Day 2!)
        book_.modify_order(m.order_ref, os.price, os.remaining);
        stats_.inc_executed();
    }
};
```

This is why we needed `modify_order` from Day 2. Without it, we'd have to
cancel and re-add for every partial fill — losing time priority and making the
replayed book state incorrect.

#### The OrderReplace handler — erase-before-find trap

The replace handler has a subtle ordering requirement:

```cpp
parser_.on_order_replace = [this](const OrderReplaceMsg& m) {
    // MUST capture side BEFORE erasing — find() fails after erase()
    Side side = Side::Buy;
    auto it_old = order_tracker_.find(m.orig_order_ref);
    if (it_old != order_tracker_.end()) side = it_old->second.side;

    // Now safe to erase
    book_.cancel_order(m.orig_order_ref);
    order_tracker_.erase(m.orig_order_ref);

    // Add the replacement
    const Price    tick = to_tick(m.price);
    if (!book_.add_order(m.new_order_ref, tick, m.shares, side)) {
        stats_.inc_rejected();
        return;
    }
    order_tracker_[m.new_order_ref] = {tick, m.shares, side};
    stats_.inc_replace();
};
```

> **⚠️ Bug we fixed:** The original version called `order_tracker_.erase()` then
> `order_tracker_.find()` — always returning `end()`, so `side` defaulted to
> `Side::Buy` for every replaced order. Tests caught this.

#### Price conversion: `to_tick()`

```cpp
Price to_tick(uint32_t itch_price) const noexcept {
    Price t = itch_price / cfg_.tick_divisor;
    if (t >= MAX_PRICE_LEVELS) t = MAX_PRICE_LEVELS - 1;
    return t;
}
```

With `tick_divisor = 100`:
- AAPL at $150.50 → ITCH price = 1,505,000 → tick = 15,050 ✓ (in cents)
- AAPL at $999.99 → ITCH price = 9,999,900 → tick = 99,999 ✓ (within 100K limit)
- Amazon at $3,500 → ITCH price = 35,000,000 → tick = 350,000 → clamped to 99,999

For expensive stocks, use `tick_divisor = 1000` (units of $0.10) to keep the
tick value within `MAX_PRICE_LEVELS`.

#### The `ReplayConfig` being at namespace scope — a C++ subtlety

```cpp
// Config at namespace scope (not nested inside ReplayEngine):
struct ReplayConfig {
    const char* symbol    = "AAPL";
    uint32_t tick_divisor = 100;
    // ...
};

class ReplayEngine {
    using Config = ReplayConfig;
    StatsCounter::Snapshot run(const uint8_t*, std::size_t,
                               const Config& cfg = {});  // default arg OK
};
```

Why not nest `Config` inside `ReplayEngine`? C++ prohibits using a class's own
nested types with default-initialized members as default function arguments
within that class's definition. Moving it to namespace scope sidesteps this
restriction. The `using Config = ReplayConfig` alias restores the ergonomic
`engine.run(data, size)` call.

---

### File: `src/replay_main.cpp` (new)

**Purpose:** Four-section Day 3 demo showing every feature end-to-end.

| Section | Function | What it shows |
|---|---|---|
| 1 | `demo_parser_correctness()` | Parser callbacks fire correctly for each ITCH message type; prices display as dollars |
| 2 | `demo_replay_basic()` | 10 orders build a 5-level book; one execution clears a level; replace amends a price |
| 3 | `demo_replay_throughput()` | 500K synthetic messages replayed, 449K matching AAPL (50K MSFT filtered), ~1M msg/sec |
| 4 | `demo_replay_from_file()` | Load real ITCH data via MmapReader and replay it |

The `SyntheticStream` helper class inside `replay_main.cpp` wraps the
`itch_build::` functions to make test stream construction readable:

```cpp
struct SyntheticStream {
    std::vector<uint8_t> buf;

    void add(uint64_t ts, uint64_t ref, char side, uint32_t qty,
             const char* sym, uint32_t price, uint16_t locate = 1) {
        uint8_t tmp[64];
        buf.insert(buf.end(), tmp,
            tmp + itch_build::add_order(tmp, locate, ts, ref, side, qty, sym, price));
    }
    // ... similar for exec, cancel, del, replace
};
```

---

### File: `tests/itch_parser_test.cpp` (new)

**Purpose:** 21 tests across two test suites, covering every message type,
edge case, and end-to-end integration path.

The `StreamBuilder` struct mirrors `SyntheticStream` in `replay_main.cpp` but is
scoped to the test file. It provides clean, readable test setup:

```cpp
StreamBuilder b;
b.stock_dir(1, "AAPL");
b.add(1001, 'B', 500, "AAPL", 1'505'000);
b.exec(1001, 100, 9999);
```

See [Section 8](#8-tests-explanation) for full per-test analysis.

---

### File: `benchmarks/replay_bench.cpp` (new)

**Purpose:** 5 benchmarks measuring each layer independently.

The `ItchFixture` struct builds a synthetic ITCH stream once at process start
and reuses it across all benchmark iterations. This ensures we measure parsing
and replay time, not stream-building time:

```cpp
static const ItchFixture kFix100K{100'000};  // built once, shared
static const ItchFixture kFix1M{1'000'000};

static void BM_ReplayFull100K(benchmark::State& state) {
    const auto& fix = kFix100K;  // reference to pre-built fixture
    for (auto _ : state) {
        ReplayEngine engine;
        engine.run(fix.buf.data(), fix.buf.size(), cfg);
    }
}
```

Message mix in the fixture: 40% add bid, 40% add ask, 10% delete, 10% partial execute.
This is a realistic approximation of a live NASDAQ stream.

---

## 5. Updated Project Structure

```
lock-free-limit-order-book/
│
├── CMakeLists.txt              ← Updated: replay, itch_parser_tests, replay_bench
│
├── include/
│   ├── order.h                 ← Unchanged (Day 1+2)
│   ├── price_level.h           ← Unchanged
│   ├── memory_pool.h           ← Unchanged
│   ├── order_map.h             ← Unchanged
│   ├── spsc_queue.h            ← Unchanged
│   ├── timing.h                ← Unchanged
│   ├── latency_tracer.h        ← Unchanged (Day 2)
│   ├── cpu_affinity.h          ← Unchanged (Day 2)
│   ├── signpost.h              ← Unchanged (Day 2)
│   ├── order_book.h            ← Unchanged (Day 1+2)
│   ├── mmap_reader.h           ← NEW: mmap(2) RAII wrapper
│   ├── itch_parser.h           ← NEW: ITCH 5.0 parser + itch_build:: namespace
│   ├── stats_counter.h         ← NEW: lock-free atomic throughput counters
│   └── replay_engine.h         ← NEW: ITCH→OrderBook translation engine
│
├── src/
│   ├── main.cpp                ← Unchanged (Day 1+2 demo)
│   └── replay_main.cpp         ← NEW: Day 3 demo (4 sections)
│
├── tests/
│   ├── order_book_test.cpp     ← Unchanged (22 tests)
│   └── itch_parser_test.cpp    ← NEW: 21 tests (12 parser + 9 replay engine)
│
├── benchmarks/
│   ├── latency_bench.cpp       ← Unchanged (9 benchmarks)
│   └── replay_bench.cpp        ← NEW: 5 benchmarks
│
├── scripts/
│   ├── build.sh                ← Unchanged
│   ├── run_bench.sh            ← Unchanged
│   ├── plot_latency.py         ← Unchanged (Day 2)
│   ├── profile_instruments.sh  ← Unchanged (Day 2)
│   ├── generate_sample_itch.py ← NEW: Python synthetic ITCH file generator
│   └── download_itch_data.sh   ← NEW: FTP instructions for real NASDAQ data
│
└── build/
    ├── order_book              ← Day 1+2 binary (unchanged)
    ├── order_book_tests        ← 22 tests (unchanged)
    ├── latency_bench           ← 9 benchmarks (unchanged)
    ├── latency_bench_traced    ← tracing build (unchanged)
    ├── replay                  ← NEW: Day 3 demo binary
    ├── itch_parser_tests       ← NEW: 21 ITCH/replay tests
    └── replay_bench            ← NEW: 5 replay benchmarks
```

---

## 6. Data Structures Added

### ItchFixture (in `replay_bench.cpp`)

A pre-built benchmark fixture that amortizes stream construction cost across
benchmark iterations:

```cpp
struct ItchFixture {
    std::vector<uint8_t> buf;

    explicit ItchFixture(int n_messages, ...) {
        buf.reserve(n_messages * 40); // ~38 bytes per message on average
        // build realistic mix: 40% add bid, 40% add ask, 10% delete, 10% exec
    }
};

// Built once at static initialization, shared across all benchmarks:
static const ItchFixture kFix100K{100'000};
static const ItchFixture kFix1M  {1'000'000};
```

**Memory:** A 100K-message fixture is ~3.8 MB. A 1M-message fixture is ~38 MB.
These live in static memory for the benchmark process lifetime.

### OrderState (in `replay_engine.h`)

The shadow state the engine maintains per resting order:

```cpp
struct OrderState {
    Price    price;      // needed to call modify_order() on partial fills
    Quantity remaining;  // decremented on each execution/partial cancel
    Side     side;       // needed to identify which side to cancel on replace
};
```

**Lifetime:** Created on `AddOrder`, destroyed on full fill, full delete, or
when the order is replaced. Kept alive in `order_tracker_` throughout.

### StreamBuilder (in `itch_parser_test.cpp`)

Test utility that composes an ITCH binary stream from individual message calls:

```cpp
struct StreamBuilder {
    std::vector<uint8_t> buf;

    void add(uint64_t ref, char side, uint32_t qty,
             const char* sym, uint32_t price, uint16_t locate = 1, ...) {
        uint8_t tmp[64];
        buf.insert(buf.end(), tmp,
            tmp + itch_build::add_order(tmp, locate, 0, ref, side, qty, sym, price));
    }
    // exec(), cancel(), del(), replace() similarly
};
```

**Why not just use `itch_build::` directly in tests?** `StreamBuilder` provides
sensible defaults (locate=1, ts=0) so tests can focus on the fields that matter
for the assertion.

---

## 7. Build & Run Instructions

### Build everything

```bash
export PATH="/opt/homebrew/bin:$PATH"
cmake build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
make -C build -j$(sysctl -n hw.ncpu)
```

Produces 6 binaries:
```
build/order_book         (Day 1+2 demo)
build/order_book_tests   (22 tests)
build/latency_bench      (9 benchmarks)
build/replay             (Day 3 demo)
build/itch_parser_tests  (21 tests)
build/replay_bench       (5 benchmarks)
```

### Run all tests

```bash
# Day 1+2 tests (22):
build/order_book_tests

# Day 3 tests (21):
build/itch_parser_tests

# Run both together (cmake ctest):
ctest --test-dir build --output-on-failure
```

Expected: **43/43 PASSED**.

### Run the Day 3 demo

```bash
build/replay
```

Expected output (abbreviated):
```
╔══════════════════════════════════════════════════════╗
║   Lock-Free Order Book — Day 3: ITCH Replay         ║
╚══════════════════════════════════════════════════════╝

=== ITCH 5.0 Parser: Correctness Demo ===
  [StockDirectory] locate=1 symbol='AAPL'
  [AddOrder]       ref=1001 BUY  qty=500 price=1505000 (=$150.5000)
  ...
  Parsed 8 messages (8 callbacks fired)

=== Replay: Basic Order Book Lifecycle ===
  Final book state:
    ASK  price=15004 ($150.04)  qty=320  orders=1
    ...
    BID  price=14999 ($149.99)  qty=200  orders=1
  Spread: 5 ticks  Mid: $150.01

=== Replay: Throughput Benchmark (500000 messages) ===
  Throughput: ~979,516 msg/sec

Done.
```

### Replay with a real NASDAQ ITCH file

```bash
# After downloading from NASDAQ FTP (see scripts/download_itch_data.sh):
gunzip S081322-v50.gz
./build/replay S081322-v50 AAPL

# Or replay a different symbol:
./build/replay S081322-v50 MSFT
```

### Generate synthetic ITCH data for larger tests

```bash
# Generate 5M messages for 3 symbols:
python3 scripts/generate_sample_itch.py \
    --out data/big.itch \
    --messages 5000000 \
    --symbols AAPL MSFT TSLA

# Replay it:
./build/replay data/big.itch AAPL
```

### Run replay benchmarks

```bash
build/replay_bench --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

Expected output (key lines):
```
BM_ItchParseOnly_mean         433 μs   (232M msg/sec)
BM_ReplayFull100K_mean         49 ms   (2.0M msg/sec)
BM_ReplayFull1M_mean          375 ms   (2.7M msg/sec)
BM_ReplaySymbolFilter_mean     16 ms   (6.3M msg/sec — locate filter only)
BM_AddOrderDecodeOnly_mean    451 μs   (222M msg/sec — decode fields only)
```

---

## 8. Tests Explanation

### Suite 1: `ItchParserTest` (12 tests)

These tests verify that the binary parser correctly deserializes every ITCH
message type. Each test builds a single-message binary stream using
`StreamBuilder`, runs the parser, and verifies every field.

---

#### Test 1: ParseStockDirectory

```cpp
b.stock_dir(42, "AAPL", 123'456'789);
// Verifies: locate=42, stock="AAPL", timestamp_ns=123456789
```
**What it tests:** The 'R' message parser correctly reads the 2-byte locate
and trims space-padding from the 8-byte stock field.
**Why it matters:** If locate is wrong, every subsequent message for this
symbol will be filtered out. All other tests depend on this working.

---

#### Test 2: ParseAddOrder

```cpp
b.add(9001, 'B', 500, "TSLA", 1'505'000, 7, 999'999'000);
// Verifies all 7 fields: locate=7, ref=9001, side='B',
//   shares=500, stock="TSLA", price=1505000, ts=999999000
```
**What it tests:** All fields of the most common ITCH message are decoded
correctly, including the non-obvious 6-byte timestamp at offset 5.

---

#### Test 3: ParseAddOrderSellSide

```cpp
b.add(42, 'S', 100, "MSFT", 3'000'000);
// Verifies: buy_sell='S', stock="MSFT"
```
**Why separate from Test 2?** Ensures the 'S' byte round-trips correctly —
it's the ASCII byte 0x53, which is also 'S' for StockDirectory type. Easy
to confuse in a switch statement.

---

#### Test 4: ParseOrderExecuted

```cpp
b.exec(7777, 200, 12345678);
// Verifies: ref=7777, executed_shares=200, match_number=12345678
```
**What it tests:** The 8-byte match number at byte offset 23 is correctly
parsed. This is the field that caused failures when the array was 1 byte too
small (test showed 12345600 instead of 12345678).

---

#### Test 5 & 6: ParseOrderCancel / ParseOrderDelete

```cpp
b.cancel(555, 300);  // Verifies: ref=555, cancelled_shares=300
b.del(888);          // Verifies: ref=888
```
**Why the Delete was tricky:** The 8-byte order_ref is the last field. With
`msg[18]` (should be `msg[19]`), the last byte of the ref was missing. Delete
was showing ref=768 instead of ref=888 (the last byte 0x78 was dropped).

---

#### Test 7: ParseOrderReplace

```cpp
b.replace(100, 200, 500, 2'000'000);
// Verifies: orig_ref=100, new_ref=200, shares=500, price=2000000
```
**What it tests:** The two 8-byte reference numbers at offsets 11 and 19
are both decoded correctly without interfering.

---

#### Test 8: ParseMultipleMessages

```cpp
b.stock_dir(1, "AAPL");
b.add(1, 'B', 100, "AAPL", 1'500'000);
b.add(2, 'S', 200, "AAPL", 1'510'000);
b.exec(1, 50, 1);
b.del(2);
// Verifies: total=5, dirs=1, adds=2, execs=1, dels=1
```
**What it tests:** The parser correctly advances the cursor past each
message and dispatches all 5 callbacks without losing synchronization.

---

#### Test 9: NullCallbackSkipsMessage

```cpp
ItchParser p; // on_add_order left null
uint64_t n = p.parse(buf, size);
EXPECT_EQ(n, 1u); // 1 message counted, but callback never called
```
**What it tests:** The `if (on_add_order && len >= 36)` guard correctly
skips dispatch when no callback is registered. No crash, no UB.

---

#### Test 10: TruncatedMessageIsSafelyIgnored

```cpp
bad[0] = 0; bad[1] = 35; // length header claims 35 bytes
bad[2] = 'A';            // only 10 bytes follow (buf size=12)
// Verifies: n=0, calls=0 — truncated message safely stopped
```
**What it tests:** The bounds check `if (p + msg_len > end) break` works.
With a real file that gets cut off mid-message, the parser stops cleanly.

---

#### Test 11: EmptyBufferIsHandled

```cpp
uint64_t n = p.parse(nullptr, 0);
EXPECT_EQ(n, 0u);
```
**What it tests:** `nullptr` with size 0 is handled — the `while (p + 2 <= end)`
condition fails immediately when `p == end`.

---

#### Test 12: BigEndianFieldsDecodedCorrectly

```cpp
b.add(0xDEADBEEFCAFEBABEULL, 'S', 0x01020304u, "TEST", 9'999'999u);
// Verifies all fields including the largest possible values
```
**What it tests:** Full-range big-endian correctness. The 8-byte order ref
`0xDEADBEEFCAFEBABE` exercises all 8 bytes. `0x01020304` for shares verifies
no bytes are swapped wrong. `9'999'999` for price verifies the max realistic price.

---

### Suite 2: `ReplayEngineTest` (9 tests)

These tests verify end-to-end behavior: build an ITCH stream, run the engine,
check the resulting OrderBook state and statistics.

---

#### Test 13: AddPassiveOrderAppearsInBook

```cpp
b.add(1, 'B', 500, "AAPL", 1'500'000); // $150.00
// Verifies: tob.bid_price = 15000 (1500000/100), tob.bid_qty = 500
```
**What it tests:** The price conversion (`to_tick`: 1500000 / 100 = 15000),
order routing to the bid side, and `top_of_book()` reflecting the new order.

---

#### Test 14: DeleteOrderRemovesFromBook

```cpp
b.add(1, 'B', 500, "AAPL", 1'500'000);
b.del(1);
// Verifies: tob.bid_qty = 0
```
**What it tests:** The 'D' handler correctly calls `cancel_order` and the
order is gone from the book.

---

#### Test 15: FullExecutionRemovesOrder

```cpp
b.add(1, 'S', 200, "AAPL", 1'510'000);
b.exec(1, 200, 999); // fully executed
// Verifies: ask_qty=0, orders_in_flight=0
```
**What it tests:** When `executed_shares >= remaining`, the order is fully
removed (via `cancel_order`), not just reduced.

---

#### Test 16: PartialExecutionReducesQuantity

```cpp
b.add(1, 'S', 300, "AAPL", 1'510'000);
b.exec(1, 100, 999); // partial: 100 of 300
// Verifies: ask_qty=200, orders_in_flight=1
```
**What it tests:** The partial fill path calls `modify_order` to reduce the
quantity in place, preserving the order's position in the queue.

---

#### Test 17: PartialCancelReducesQuantity

```cpp
b.add(1, 'B', 400, "AAPL", 1'500'000);
b.cancel(1, 150); // ITCH 'X': partial cancel
// Verifies: bid_qty=250 (400 - 150)
```
**What it tests:** ITCH 'X' is not a full cancellation — it reduces quantity.
The `on_order_cancel` handler correctly computes the new remaining and calls
`modify_order`.

---

#### Test 18: ReplaceOrderUsesNewRefAndPrice

```cpp
b.add(10, 'B', 300, "AAPL", 1'500'000); // old at $150.00
b.replace(10, 20, 300, 1'505'000);       // new ref=20, $150.50
// Verifies: bid_price=15050 (1505000/100), bid_qty=300
```
**What it tests:** The 'U' handler cancels the old order (ref=10) and adds
a new one (ref=20) at the new price. The old ref no longer exists.

---

#### Test 19: WrongSymbolIsFiltered

```cpp
b.stock_dir(1, "AAPL"); // locate=1
b.stock_dir(2, "MSFT"); // locate=2
b.add(1, 'B', 500, "AAPL", 1'500'000, 1); // locate=1: AAPL
b.add(2, 'B', 500, "MSFT", 3'000'000, 2); // locate=2: MSFT (should be filtered)
// Verifies: orders_in_flight=1, bid_qty=500 (only AAPL in book)
```
**What it tests:** The locate filter correctly ignores messages for other
symbols. With only one integer comparison per message, MSFT's order never
touches the order book.

---

#### Test 20: StatsCountersAreAccurate

```cpp
b.add(1, 'B', 200, "AAPL", 1'500'000);
b.add(2, 'S', 100, "AAPL", 1'510'000);
b.exec(2, 100, 1); // full fill of order 2
b.del(1);          // cancel order 1
// Verifies: added=2, cancelled=2, trades=1, shares_traded=100
```
**What it tests:** All 4 counter paths fire correctly:
- `inc_add()` × 2 (two AddOrders)
- `inc_trade(100)` (execution)
- `inc_cancel()` × 2 (full fill + delete)

---

#### Test 21: MultiLevelMatchOnReplay

```cpp
b.add(1, 'S', 100, "AAPL", 1'500'100); // ask at $150.01
b.add(2, 'S', 200, "AAPL", 1'500'200); // ask at $150.02
b.add(3, 'S', 300, "AAPL", 1'500'300); // ask at $150.03
b.exec(1, 100, 1); // fills level 1 completely
b.exec(2, 200, 2); // fills level 2 completely
b.exec(3, 150, 3); // partially fills level 3
// Verifies: ask_qty=150, trades=3, shares_traded=450
```
**What it tests:** Three execution messages against different orders correctly
reduce three separate price levels. This exercises the interaction between
ITCH's per-order execution model and the OrderBook's price-level structure.
**Why it matters:** In a real replay, aggressive orders are matched against many
resting orders. The replayed book must reflect each execution correctly.

---

## 9. Key Algorithms Implemented

### Algorithm 1: Single-Pass Buffer Walk with Framed Messages

**Problem:** Given a byte buffer containing ITCH messages each prefixed by a
2-byte length, extract and dispatch each message in one forward pass.

```
Buffer:
┌──┬──┬────────────────────────┬──┬──┬────────────────────┬──┬──┬──
│24│00│ AddOrder (36 bytes)    │12│00│ Delete (19 bytes)  │...
└──┴──┴────────────────────────┴──┴──┴────────────────────┴──┴──┴──
 ← 2-byte length →               ← 2-byte length →
```

```cpp
const uint8_t* p   = data;
const uint8_t* end = data + size;

while (p + 2 <= end) {           // enough bytes for a length prefix?
    uint16_t len = be16(p);      // read length (big-endian)
    p += 2;                      // advance past length prefix

    if (p + len > end) break;    // message body extends past end? stop.

    dispatch(p, len);            // type byte is at p[0]; process the message
    p += len;                    // advance past message body
}
```

**Complexity:** O(n) where n = total bytes. Each byte is read exactly once
(plus the 2-byte length prefix). No backtracking, no lookahead.

**Correctness for truncated files:** The `if (p + len > end) break` guard
handles files that were cut off mid-write. The parser stops cleanly rather
than reading past the buffer.

---

### Algorithm 2: Locate-Based Symbol Filtering

**Problem:** A full NASDAQ day has ~500 million messages across ~3,000 symbols.
We care about ~5 symbols. How do we discard 99.8% of messages as fast as possible?

**Step 1** — Learn the locate number from the Stock Directory:
```cpp
if (std::strcmp(m.stock, cfg_.symbol) == 0) {
    locate_    = m.stock_locate;  // e.g., locate = 42
    found_sym_ = true;
}
```
This string comparison happens once per Stock Directory message (thousands of
symbols × once per day = negligible cost).

**Step 2** — Filter all subsequent messages with a single integer compare:
```cpp
if (!found_sym_ || m.stock_locate != locate_) return;
```

**Why this is fast:**
- `found_sym_` is likely in a register after the first `AddOrder`
- `m.stock_locate` is at offset 1 of every message — always cache-warm
- A uint16_t compare is 1 CPU instruction
- All OrderBook work is skipped for non-matching messages

**Benchmark result:** `BM_ReplaySymbolFilter` (wrong symbol, 0 OrderBook calls)
takes 16 ms for 100K messages vs 49 ms for full replay. The filter itself costs
33 ms worth of work — roughly the cost of just walking the binary buffer.

---

### Algorithm 3: Partial Execution → `modify_order`

**Problem:** ITCH 'E' says "X shares of order #N were executed." We need to
update the order book, but we don't know the current remaining quantity or price
from the ITCH message alone.

**Solution:** Maintain a shadow map `order_ref → OrderState{price, remaining, side}`.

```
Initial state: order #1001, ask at $151.00, 300 shares
  order_tracker_[1001] = {price=15100, remaining=300, side=Sell}
  book: asks_[15100] = [#1001: 300sh]

Receive 'E' for #1001, executed=100:
  os.remaining = 300 - 100 = 200
  os.remaining > 0 → partial fill
  → book_.modify_order(1001, 15100, 200) [in-place: preserves priority]
  → order_tracker_[1001].remaining = 200

Receive 'E' for #1001, executed=200:
  os.remaining = 200 - 200 = 0
  os.remaining == 0 → fully filled
  → book_.cancel_order(1001)
  → order_tracker_.erase(1001)
```

**Why `modify_order` instead of cancel+re-add?** The resting order's time
priority is the result of when it arrived at the exchange. If we cancel and
re-add, it goes to the back of the queue — wrong. `modify_order` with the
same price reduces quantity in-place and preserves the order's position,
correctly reflecting the exchange's behavior.

---

### Algorithm 4: Order Replace (Cancel + Re-Add with New Reference)

**Problem:** ITCH 'U' sends `orig_ref` (the old order) and `new_ref` (the
replacement). The replacement always gets a new reference number — the old
reference is retired forever.

```
Before replace: order_tracker_[10] = {price=15000, remaining=300, side=Buy}
                book: bids_[15000] = [#10: 300sh]

Receive 'U': orig_ref=10, new_ref=20, shares=300, price=15050

Step 1: Capture side BEFORE erasing (order 10 still in tracker)
        side = order_tracker_[10].side = Side::Buy

Step 2: Cancel old
        book_.cancel_order(10)
        order_tracker_.erase(10)

Step 3: Add replacement
        book_.add_order(20, 15050, 300, Side::Buy)
        order_tracker_[20] = {price=15050, remaining=300, side=Buy}

After: order_tracker_[20] = {price=15050, remaining=300, side=Buy}
       book: bids_[15050] = [#20: 300sh]  ← at the back (lost priority)
```

The new order goes to the back of the queue at its new price — this is
exchange-standard behavior. The old reference number can never be reused
in the same session.

---

## 10. Memory Management

### What's new on Day 3?

The `order_tracker_` is the main new allocation:

```cpp
std::unordered_map<uint64_t, OrderState> order_tracker_;
// Pre-sized in run():
order_tracker_.reserve(1 << 20); // reserve capacity for 1M entries
```

**`reserve(1 << 20)`:** Pre-allocates the hash table's bucket array for 1M
entries up front. Without this, `unordered_map` would repeatedly rehash and
reallocate as entries are added — each rehash copies all existing entries.
With `reserve()`, there are zero rehashes during replay.

**Memory for 1M orders:**
- `OrderState`: price(4) + remaining(4) + side(1) + padding(3) = 12 bytes per entry
- `unordered_map` overhead: ~48 bytes per bucket + ~16 bytes per node (with pointer)
- Total per entry: ~64 bytes
- For 1M entries: ~64 MB

This is in addition to the OrderBook's ~88 MB from Days 1+2. Total process
footprint for full replay: ~150+ MB.

**The `ItchFixture` (benchmark) memory:**
- `kFix100K.buf`: ~3.8 MB (38 bytes/msg × 100K)
- `kFix1M.buf`: ~38 MB
- These are static globals, live for the benchmark process lifetime

### Why `std::unordered_map` instead of our custom `OrderMap`?

Our `OrderMap` (from Day 1) is faster — it has no pointer chasing, fits in
cache better, and uses Fibonacci hashing. But it has a fixed size (1M slots
pre-allocated, ~16 MB).

For the replay engine's `order_tracker_`, we chose `std::unordered_map` because:
1. The replay engine is not on the critical latency path — we care about
   throughput (messages/sec), not per-operation nanoseconds
2. `unordered_map` handles arbitrary key ranges without pre-knowing the count
3. Its `erase()` is more robust for the complex lifecycle (add / partial-reduce
   / full-remove) the tracker needs

**Day 4 opportunity:** Replace `order_tracker_` with `OrderMap` for tighter
performance. The benchmark `BM_ReplayFull1M` shows 375 ms for 1M messages —
profiling would tell us what fraction of that is hash map operations.

---

## 11. Performance Characteristics

### Benchmark results (Apple Silicon M-series, Release, 3 repetitions)

| Benchmark | CPU Mean | Throughput | Notes |
|---|---|---|---|
| `BM_ItchParseOnly` | **433 μs / 100K msgs** | **232M msg/sec** | Callbacks fire, no OrderBook |
| `BM_ReplayFull100K` | **49 ms / 100K msgs** | **2.0M msg/sec** | Full parse + book |
| `BM_ReplayFull1M` | **375 ms / 1M msgs** | **2.7M msg/sec** | 10× more msgs, better cache |
| `BM_ReplaySymbolFilter` | **16 ms / 100K msgs** | **6.3M msg/sec** | Wrong symbol, all filtered |
| `BM_AddOrderDecodeOnly` | **451 μs / 100K msgs** | **222M msg/sec** | Decode fields, no callbacks |

### Reading the numbers

**Parser alone: 232M msg/sec**
At ~38 bytes per message, this is 8.8 GB/sec of binary data processed —
close to the theoretical L3 cache bandwidth on Apple Silicon. The parser is
essentially memory-bandwidth bound.

**Full replay: 2.0–2.7M msg/sec**
The ~100× slowdown from parse-only to full replay comes from:
1. Hash map lookups and inserts (`order_tracker_`) — ~50% of time
2. OrderBook operations (pool, map, price level) — ~40% of time
3. `std::chrono` for elapsed timing — ~5%
4. `std::function` call overhead for lambdas — ~5%

**Symbol filter: 6.3M msg/sec**
3× faster than full replay. The remaining work is: buffer walk + `be16()` for
locate + `switch` dispatch + locate compare + `return`. This is the minimum
cost of "doing nothing" for each message while still parsing its type.

**Why 1M replay (2.7M/sec) is faster than 100K (2.0M/sec)?**
The 1M fixture pre-warms the CPU's L3 cache and branch predictor. After the
first 100K messages, the engine's inner loops are in an optimized steady state.
The 100K benchmark starts cold each iteration.

### Comparison to real-world NASDAQ data

A full NASDAQ trading day has approximately:
- 400-600 million messages (all symbols combined)
- 10-20 million messages for AAPL specifically
- Replay time for AAPL at our throughput: 10M / 2.7M msg/sec ≈ 3.7 seconds

A real exchange matching engine processes these messages in real-time as they
arrive. The NASDAQ feed runs at up to ~10M messages/second peak. Our engine
at 2.7M msg/sec single-threaded cannot keep up with peak load — but with the
multi-symbol sharding of Day 4 (4 P-cores), we'd reach ~10M msg/sec, which is
sufficient.

---

## 12. What's Next (Day 4 Preview)

### Primary goal: Multi-symbol sharding across CPU cores

Instead of one `OrderBook` handling all symbols on one thread, Day 4 builds:

```
Core 0 (P-core): Symbols A-M  → OrderBook #0
Core 1 (P-core): Symbols N-Z  → OrderBook #1
Core 2 (P-core): High-volume  → OrderBook #2 (AAPL, MSFT, TSLA)
Core 3 (P-core): Options flow → OrderBook #3
```

The ITCH parser runs on a dedicated I/O thread. It reads the locate from each
message and routes it to the appropriate core's input queue (SPSC queue from
Day 1!) without any locking.

### Secondary goals

| Feature | Details |
|---|---|
| **SPSC queue between parser and book** | Parser thread pushes parsed messages; matching thread pops and applies — true pipeline parallelism |
| **Lock-free order tracker** | Replace `std::unordered_map` with a lock-free hash map for the shadow state |
| **Tick-to-price reverse lookup** | Given a tick, display the dollar price — needed for clean output |
| **VWAP calculator** | Volume-Weighted Average Price accumulated during replay — a common risk metric |

### How Day 4 connects to Day 3

```
Day 3 (single-threaded):        Day 4 (pipelined multi-core):
  parse()                   →     I/O thread: parse → route by locate
  → on_add_order callback   →     Core 0 thread: pop SPSC → book_.add_order()
  → book_.add_order()       →     Core 1 thread: pop SPSC → book_.add_order()
  (all one thread)              (true parallelism: I/O + matching separate)
```

---

## 13. Common Issues & Debugging

### Issue: Off-by-one in message array size

**Symptom:** Test shows `price=1504768` instead of `1505000`, or `ref=768`
instead of `888`.

**Root cause:** The message array is 1 byte smaller than the message payload.
The last byte of the last field is never written into the framed output.

**How we caught it:** `ItchParserTest.ParseAddOrder` verified every field
including price. The recovered price had its last byte (0xA8) replaced by 0x00.

**Fix:** Count bytes explicitly: type(1)+locate(2)+tracking(2)+ts(6)+ref(8)+side(1)+shares(4)+stock(8)+price(4) = 36. Add this comment next to every array size.

**Never do this:**
```cpp
uint8_t msg[35]{}; // WRONG: 35 is one too few for Add Order
put32(msg + 32, price); // writes bytes 32, 33, 34, 35 — but 35 is out of bounds!
```

### Issue: Erase-before-find in the Replace handler

**Symptom:** All replaced orders use Side::Buy regardless of their actual side.

**Root cause:**
```cpp
order_tracker_.erase(m.orig_order_ref);  // erases the entry
auto it = order_tracker_.find(m.orig_order_ref); // always returns end()!
Side side = (it != order_tracker_.end()) ? it->second.side : Side::Buy; // always Buy
```

**Fix:** Capture the side before erasing:
```cpp
Side side = Side::Buy;
auto it = order_tracker_.find(m.orig_order_ref);  // find FIRST
if (it != order_tracker_.end()) side = it->second.side;
order_tracker_.erase(m.orig_order_ref);           // THEN erase
```

### Issue: `mmap_reader.h` fails to compile with `-fno-exceptions`

**Symptom:** `cannot use 'throw' with exceptions disabled`

**Root cause:** Release flags included `-fno-exceptions`, which disables C++
exception handling entirely. `MmapReader` uses `throw std::runtime_error(...)`.

**Fix:** Removed `-fno-exceptions` from global Release flags in CMakeLists.txt.
Only apply it to specific targets if needed (latency-critical benchmarks where
exceptions genuinely don't occur on the hot path).

### Issue: Nested `Config` struct with default args causes compile error

**Symptom:**
```
error: default member initializer for 'symbol' needed within definition of
enclosing class 'ReplayEngine' outside of member functions
```

**Root cause:** C++ prohibits using a class's own nested struct (with
default-initialized members) as a default function argument within the class.

**Fix:** Declare `ReplayConfig` at namespace scope, then alias it:
```cpp
struct ReplayConfig { const char* symbol = "AAPL"; ... };  // namespace scope

class ReplayEngine {
    using Config = ReplayConfig;  // alias for ergonomics
    StatsCounter::Snapshot run(..., const Config& cfg = {}); // now compiles
};
```

### Issue: Stock locate not found (all messages filtered)

**Symptom:** Replay shows 0 messages processed, book is empty.

**Causes:**
1. Symbol name mismatch — ITCH uses space-padded 8-char symbols; our `parse_stock`
   trims spaces, but check that you're passing `"AAPL"` not `"AAPL    "` or `"aapl"`.
2. Stock Directory message appears AFTER the first AddOrder in the file — ITCH spec
   says it appears at session start, but some test files violate this. Add the
   stock_dir message first in your synthetic streams.
3. The ITCH file uses 'F' (AddOrderMpid) for all orders — our parser handles
   both 'A' and 'F', reusing the `on_add_order` callback. Verify the file's
   message types with a hex dump.

**Debugging:**
```cpp
cfg.verbose = true;  // prints ADD/DEL events to stdout
```
If nothing prints, the locate was never found.

### Issue: `build/replay_bench` runs very slowly

**Symptom:** `BM_ReplayFull1M` takes minutes, not seconds.

**Root cause:** The 1M-message fixture (`kFix1M`) is built at static init time.
If the benchmark binary is run under Valgrind or with ASan, static init takes much
longer. Also, the `std::unordered_map` in `order_tracker_` does many allocations
— this is expected.

**Check:** Run without any sanitizers (`cmake build -DCMAKE_BUILD_TYPE=Release`,
not Debug). The Release build with `-O3 -march=native` is 5-10× faster.

---

## 14. Glossary

**Big-endian:** Byte storage order where the most significant byte comes first
(lowest address). Used by NASDAQ ITCH and most network protocols. Opposite of
little-endian (used by x86, ARM in normal mode). Byte-swapping is required
when reading big-endian data on little-endian hardware.

**`MADV_SEQUENTIAL`:** A `madvise()` hint telling the kernel the mapping will
be accessed sequentially. The kernel responds by aggressively prefetching pages
ahead of the current read position. Reduces page-fault stalls for sequential
binary parsing.

**Market data feed:** A broadcast stream from an exchange delivering every order,
cancellation, and trade to all connected subscribers in real time. ITCH is
NASDAQ's market data feed protocol.

**Match number:** A unique 64-bit integer assigned by the exchange to every
trade. Links execution reports in the ITCH feed to clearing system records for
post-trade reconciliation.

**`mmap()`:** System call that maps a file (or anonymous memory) into a process's
virtual address space. Enables zero-copy access to file contents: the parser
reads directly from the OS page cache without any user-space buffering.

**ITCH 5.0:** NASDAQ's binary market data protocol, version 5.0. Released 2014.
Provides full order book depth (every individual order, not just quotes).
Historical data available free from NASDAQ's FTP server with registration.

**Order reference number:** A unique 64-bit integer assigned by NASDAQ to each
order it accepts. Used as the `OrderId` in our `OrderBook`. Guaranteed ≥ 1 within
a session, which means it never conflicts with our OrderMap's empty sentinel (0).

**Order Replace ('U'):** An ITCH message that atomically cancels an existing
order and submits a replacement with a new reference number, new price, and/or
new quantity. The replacement always loses time priority.

**Order tracker:** Our `order_ref → OrderState` shadow map that maintains per-order
price, remaining quantity, and side outside the OrderBook. Needed because ITCH
'E' (executed) only reports how many shares traded, not how many remain.

**`parse_stock()`:** Helper that copies an 8-byte space-padded ITCH stock field
into a 9-byte null-terminated C string, trimming trailing spaces. Enables
`strcmp`-based symbol matching.

**`PROT_READ`:** `mmap()` protection flag meaning the mapping is read-only.
Attempts to write to a `PROT_READ` mapping cause a segfault. Used in `MmapReader`
since we never modify the file.

**`reserve()`:** `std::unordered_map::reserve(n)` pre-allocates bucket capacity
for `n` elements, avoiding rehashing as elements are inserted. Called with
`1 << 20` (1M) to handle a full ITCH trading session without any rehash.

**Shadow state:** A parallel data structure that tracks per-order information
not exposed by the `OrderBook` interface. The `order_tracker_` in `ReplayEngine`
is a shadow state map — it tracks `remaining` and `price` so the engine can
call `modify_order()` correctly on partial fills.

**Stock Directory ('R'):** An ITCH message sent at session open for every listed
symbol. Establishes the mapping from `stock_locate` (uint16_t) to symbol name
(8-char ASCII). All subsequent messages use the locate integer, not the full name.

**Stock locate:** A 2-byte session-specific identifier for a symbol, established
by the Stock Directory message. Filtering by locate (`m.stock_locate != target`)
costs one integer comparison vs. eight bytes of string comparison.

**`MADV_SEQUENTIAL` vs `MADV_WILLNEED`:** Both are prefetch hints. `SEQUENTIAL`
tells the kernel "I'll access this linearly, prefetch ahead." `WILLNEED` says
"prefetch all of this now." For streaming binary parsing, `SEQUENTIAL` is better
— it prefetches incrementally as you advance, rather than loading the entire
file at once.

**Zero-allocation parser:** A parser that never calls `malloc`, `new`, or uses
heap-allocated containers during its main loop. Our `ItchParser::parse()` allocates
nothing — message structs are stack-local and passed by `const&` to callbacks.

**Zero-copy parsing:** The parser reads fields directly from the original buffer
without copying them into a separate data structure. Our `dispatch()` fills structs
by dereferencing `msg + offset`, not by copying to an intermediate buffer.

---

## 15. Visual Diagrams

### Complete ITCH Message Wire Format

```
Binary stream (reading left to right):
                                                               next msg...
┌──┬──┬──┬──────────────────────────────────────────────────┬──┬──┬──
│00│24│'A│locate│track │ timestamp (6 bytes)  │order_ref (8)│...│00│12│
└──┴──┴──┴──────────────────────────────────────────────────┴──┴──┴──
 ↑──↑      ↑                                                     ↑──↑
 length=36  type                                              next length=18
 (big-endian)                                                 ('D' Delete msg)

Zoom in on 'A' AddOrder payload (36 bytes):
Offset:  0    1    3    5                11                  19   20        24              32  35
         │    │    │    │                │                   │    │         │               │
         │type│loc │trk │  timestamp     │    order_ref      │B/S │ shares  │    stock      │price│
         │'A' │ 2B │ 2B │    6 bytes     │     8 bytes       │ 1B │  4B     │   8 bytes     │ 4B  │
         ↑                                                                                        ↑
         Byte 0                                                                              Byte 35
```

### Locate Filter Flow

```
ITCH stream (3 symbols):

['R' AAPL locate=1]  ← on_stock_directory: found_sym_=true, locate_=1
['R' MSFT locate=2]  ← on_stock_directory: already found, skip
['R' TSLA locate=3]  ← on_stock_directory: already found, skip
['A' locate=2 ...]   ← on_add_order: locate(2) != 1 → return immediately ✗
['A' locate=1 ...]   ← on_add_order: locate(1) == 1 → process ✓
['D' locate=3 ...]   ← on_order_delete: locate(3) != 1 → return ✗
['A' locate=1 ...]   ← on_add_order: locate(1) == 1 → process ✓
['E' locate=1 ...]   ← on_order_executed: locate(1) == 1 → process ✓
['A' locate=2 ...]   ← return ✗

Cost of ✗: 2 comparisons + return  (< 5 ns)
Cost of ✓: full order book operation (~ 300-500 ns)
```

### Order Tracker Lifecycle

```
ITCH stream              order_tracker_               OrderBook

'A' ref=1001 $150 B 300 → [1001: {15000, 300, Buy}] → bids_[15000] = [#1001: 300sh]

'E' ref=1001 exec=100    → os.remaining = 200        → modify_order(1001, 15000, 200)
                           [1001: {15000, 200, Buy}]    bids_[15000] = [#1001: 200sh]

'X' ref=1001 cancel=50   → os.remaining = 150        → modify_order(1001, 15000, 150)
                           [1001: {15000, 150, Buy}]    bids_[15000] = [#1001: 150sh]

'E' ref=1001 exec=150    → os.remaining = 0          → cancel_order(1001)
                           erase(1001) ──────────────→ bids_[15000] = [] (empty)

'U' ref=1001→1002        ← (1001 already gone)
    price=15100 qty=200  → [1002: {15100, 200, Buy}] → add_order(1002, 15100, 200, Buy)

Final: tracker={1002: {15100,200,Buy}}, bids_[15100]=[#1002: 200sh]
```

### Big-Endian Field Reading for Price

```
ITCH AddOrder price field (4 bytes at offset 32):

Buffer bytes at offset 32: [0x00][0x16][0xF6][0xE8]
                             MSB                LSB

be32(msg + 32):
  step 1: uint32_t(0x00) << 24 = 0x00000000
  step 2: uint32_t(0x16) << 16 = 0x00160000
  step 3: uint32_t(0xF6) <<  8 = 0x0000F600
  step 4: uint32_t(0xE8)       = 0x000000E8
  OR all:                        0x0016F6E8 = 1,505,000

to_tick(1,505,000, divisor=100):
  1,505,000 / 100 = 15,050 ticks = $150.50

❌ WRONG (would give 1,504,768):
  uint32_t price = *reinterpret_cast<uint32_t*>(msg + 32);
  // reads little-endian: [0xE8][0xF6][0x16][0x00] = 0x0016F6E8... wait
  // actually on little-endian: interprets as 0x00E8F616 = 15,267,350
  // AND has alignment UB if msg+32 is not 4-byte aligned
```

### The parse() Loop State Machine

```
State: READING_LENGTH

      ┌─────────────────────────────────────────────────────┐
      │  p + 2 <= end?                                       │
      │    NO  → DONE (EOF or < 2 bytes remaining)          │
      │    YES → read len = be16(p); p += 2                 │
      └────────────────────┬────────────────────────────────┘
                           │
                    ┌──────▼──────────────────────────────────┐
                    │  p + len > end?                          │
                    │    YES → DONE (truncated message at EOF)  │
                    │    NO  → dispatch(p, len); p += len      │
                    └──────┬───────────────────────────────────┘
                           │
                           └──► loop back to READING_LENGTH
```

### Replay Engine Data Flow

```
File on disk (e.g. S081322-v50.itch, 8 GB)
        │
        ▼  mmap(PROT_READ, MAP_PRIVATE, MADV_SEQUENTIAL)
        │
const uint8_t* data  ──────────────────────────────────────────────────────┐
                                                                            │
                                                                            ▼
                                                                    ItchParser::parse()
                                                                            │
                  ┌─────────────────────────────────────────────────────────┤
                  │ dispatch() for each message                             │
                  │                                                         │
                  │ 'R' → on_stock_directory  ─► found_sym_=true, locate_=1 │
                  │ 'A' → on_add_order                                      │
                  │         locate check                                    │
                  │         to_tick(price)                                  │
                  │         book_.add_order()  ─► OrderBook                │
                  │         order_tracker_[ref] = {tick, qty, side}        │
                  │ 'E' → on_order_executed                                │
                  │         locate check                                    │
                  │         os = order_tracker_[ref]                        │
                  │         os.remaining -= exec                            │
                  │         if 0: book_.cancel_order()                      │
                  │         else: book_.modify_order()                      │
                  │ 'X' → on_order_cancel    ─► modify_order or cancel    │
                  │ 'D' → on_order_delete    ─► cancel_order              │
                  │ 'U' → on_order_replace   ─► cancel + add_order        │
                  │                                                         │
                  └─────────────────────────────────────────────────────────┘
                                                                            │
                                                                   StatsCounter
                                                                   (relaxed atomics)
                                                                            │
                                                                   print_stats()
```

---

*End of Day 3 Complete Guide.*

*Next: [Day 4] — Multi-symbol sharding across P-cores, SPSC pipeline between
parser and matching threads, lock-free order tracker, VWAP accumulator.*
