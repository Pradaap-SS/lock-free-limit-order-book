#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>

// NASDAQ ITCH 5.0 binary protocol parser.
//
// Wire format (historical/FTP files):
//   [2-byte big-endian message length][message bytes...]
//   Repeat until EOF.
//
// All multi-byte integer fields are big-endian on the wire.
// Prices are in units of $0.0001 (e.g. 1005000 = $100.50).
// Timestamps are nanoseconds since midnight.
// Stocks are 8-byte ASCII, right-padded with spaces.
//
// Reference: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf

// ── Big-endian deserialization helpers ───────────────────────────────────────

namespace itch_detail {

inline uint16_t be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t be32(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

// 6-byte big-endian timestamp (nanoseconds)
inline uint64_t be48(const uint8_t* p) noexcept {
    return (uint64_t(p[0]) << 40) | (uint64_t(p[1]) << 32) |
           (uint64_t(p[2]) << 24) | (uint64_t(p[3]) << 16) |
           (uint64_t(p[4]) << 8)  |  uint64_t(p[5]);
}

inline uint64_t be64(const uint8_t* p) noexcept {
    return (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) |
           (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32) |
           (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) |
           (uint64_t(p[6]) << 8)  |  uint64_t(p[7]);
}

// Copy 8-byte stock field and null-terminate (trims trailing spaces)
inline void parse_stock(const uint8_t* src, char* dst9) noexcept {
    std::memcpy(dst9, src, 8);
    dst9[8] = '\0';
    for (int i = 7; i >= 0 && dst9[i] == ' '; --i) dst9[i] = '\0';
}

} // namespace itch_detail

// ── Parsed message structs ────────────────────────────────────────────────────

// ITCH message type bytes
enum class ItchMsgType : uint8_t {
    StockDirectory  = 'R',
    AddOrder        = 'A',  // no MPID
    AddOrderMpid    = 'F',  // with MPID (same fields + 4-byte attribution)
    OrderExecuted   = 'E',
    OrderExecutedPx = 'C',  // executed with price (off-exchange)
    OrderCancel     = 'X',  // reduce quantity
    OrderDelete     = 'D',  // full cancel
    OrderReplace    = 'U',  // cancel + re-add at new terms
    Trade           = 'P',  // non-attributable trade
    CrossTrade      = 'Q',
    BrokenTrade     = 'B',
    SystemEvent     = 'S',
};

struct StockDirectoryMsg {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    char     stock[9];      // null-terminated, trimmed
};

struct AddOrderMsg {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref;
    char     buy_sell;       // 'B' or 'S'
    uint32_t shares;
    char     stock[9];
    uint32_t price;          // ITCH units: $0.0001 per unit
};

struct OrderExecutedMsg {
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct OrderCancelMsg {    // partial cancellation — reduces quantity
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref;
    uint32_t cancelled_shares;
};

struct OrderDeleteMsg {    // full cancellation
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t order_ref;
};

struct OrderReplaceMsg {   // cancel-and-replace at new price/qty
    uint16_t stock_locate;
    uint64_t timestamp_ns;
    uint64_t orig_order_ref;
    uint64_t new_order_ref;
    uint32_t shares;
    uint32_t price;
};

// ── Parser ────────────────────────────────────────────────────────────────────

// Zero-allocation, zero-copy streaming parser.
// Walk the buffer once; dispatch each message to a registered callback.
// Unregistered (null) callbacks are skipped with a single branch.
//
// Example:
//   ItchParser p;
//   p.on_add_order = [&](const AddOrderMsg& m) { ... };
//   uint64_t n = p.parse(buf, size);
//   printf("processed %llu messages\n", n);

class ItchParser {
public:
    // Callbacks — set to nullptr to skip that message type
    std::function<void(const StockDirectoryMsg&)> on_stock_directory;
    std::function<void(const AddOrderMsg&)>        on_add_order;
    std::function<void(const OrderExecutedMsg&)>   on_order_executed;
    std::function<void(const OrderCancelMsg&)>     on_order_cancel;
    std::function<void(const OrderDeleteMsg&)>     on_order_delete;
    std::function<void(const OrderReplaceMsg&)>    on_order_replace;

    // Process the complete ITCH binary buffer.
    // Returns the total number of messages seen (of any type).
    uint64_t parse(const uint8_t* data, std::size_t size) noexcept {
        using namespace itch_detail;
        uint64_t count = 0;
        const uint8_t* p   = data;
        const uint8_t* end = data + size;

        while (p + 2 <= end) {
            const uint16_t msg_len = be16(p);
            p += 2;

            if (p + msg_len > end) break; // truncated message — stop

            dispatch(p, msg_len);
            ++count;
            p += msg_len;
        }
        return count;
    }

private:
    void dispatch(const uint8_t* msg, uint16_t len) noexcept {
        using namespace itch_detail;
        if (len == 0) return;
        const uint8_t type = msg[0];

        switch (static_cast<ItchMsgType>(type)) {

        case ItchMsgType::StockDirectory:
            if (on_stock_directory && len >= 33) {
                StockDirectoryMsg m;
                m.stock_locate = be16(msg + 1);
                m.timestamp_ns = be48(msg + 5);
                parse_stock(msg + 11, m.stock);
                on_stock_directory(m);
            }
            break;

        case ItchMsgType::AddOrder:
            // type(1)+locate(2)+tracking(2)+ts(6)+ref(8)+side(1)+shares(4)+stock(8)+price(4) = 36
            if (on_add_order && len >= 36) {
                AddOrderMsg m;
                m.stock_locate = be16(msg + 1);
                m.timestamp_ns = be48(msg + 5);
                m.order_ref    = be64(msg + 11);
                m.buy_sell     = static_cast<char>(msg[19]);
                m.shares       = be32(msg + 20);
                parse_stock(msg + 24, m.stock);
                m.price        = be32(msg + 32);
                on_add_order(m);
            }
            break;

        case ItchMsgType::AddOrderMpid:
            // Same as AddOrder + 4-byte MPID attribution = 40 bytes
            if (on_add_order && len >= 40) {
                AddOrderMsg m;
                m.stock_locate = be16(msg + 1);
                m.timestamp_ns = be48(msg + 5);
                m.order_ref    = be64(msg + 11);
                m.buy_sell     = static_cast<char>(msg[19]);
                m.shares       = be32(msg + 20);
                parse_stock(msg + 24, m.stock);
                m.price        = be32(msg + 32);
                on_add_order(m);
            }
            break;

        case ItchMsgType::OrderExecuted:
            // type(1)+locate(2)+tracking(2)+ts(6)+ref(8)+exec_shares(4)+match(8) = 31
            if (on_order_executed && len >= 31) {
                OrderExecutedMsg m;
                m.stock_locate    = be16(msg + 1);
                m.timestamp_ns    = be48(msg + 5);
                m.order_ref       = be64(msg + 11);
                m.executed_shares = be32(msg + 19);
                m.match_number    = be64(msg + 23);
                on_order_executed(m);
            }
            break;

        case ItchMsgType::OrderExecutedPx:
            // Same as OrderExecuted + 1-byte printable + 4-byte price = 36
            if (on_order_executed && len >= 36) {
                OrderExecutedMsg m;
                m.stock_locate    = be16(msg + 1);
                m.timestamp_ns    = be48(msg + 5);
                m.order_ref       = be64(msg + 11);
                m.executed_shares = be32(msg + 19);
                m.match_number    = be64(msg + 23);
                on_order_executed(m);
            }
            break;

        case ItchMsgType::OrderCancel:
            // type(1)+locate(2)+tracking(2)+ts(6)+ref(8)+cancelled(4) = 23
            if (on_order_cancel && len >= 23) {
                OrderCancelMsg m;
                m.stock_locate     = be16(msg + 1);
                m.timestamp_ns     = be48(msg + 5);
                m.order_ref        = be64(msg + 11);
                m.cancelled_shares = be32(msg + 19);
                on_order_cancel(m);
            }
            break;

        case ItchMsgType::OrderDelete:
            // type(1)+locate(2)+tracking(2)+ts(6)+ref(8) = 19
            if (on_order_delete && len >= 19) {
                OrderDeleteMsg m;
                m.stock_locate = be16(msg + 1);
                m.timestamp_ns = be48(msg + 5);
                m.order_ref    = be64(msg + 11);
                on_order_delete(m);
            }
            break;

        case ItchMsgType::OrderReplace:
            // type(1)+locate(2)+tracking(2)+ts(6)+orig(8)+new(8)+shares(4)+price(4) = 35
            if (on_order_replace && len >= 35) {
                OrderReplaceMsg m;
                m.stock_locate   = be16(msg + 1);
                m.timestamp_ns   = be48(msg + 5);
                m.orig_order_ref = be64(msg + 11);
                m.new_order_ref  = be64(msg + 19);
                m.shares         = be32(msg + 27);
                m.price          = be32(msg + 31);
                on_order_replace(m);
            }
            break;

        default:
            break; // unknown or unhandled message type
        }
    }
};

// ── Binary builder helpers (for tests and synthetic data) ─────────────────────

namespace itch_build {

// Write big-endian values into a buffer at offset
inline void put16(uint8_t* p, uint16_t v) noexcept {
    p[0] = (v >> 8) & 0xFF;  p[1] = v & 0xFF;
}
inline void put32(uint8_t* p, uint32_t v) noexcept {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8)  & 0xFF; p[3] =  v & 0xFF;
}
inline void put48(uint8_t* p, uint64_t v) noexcept {
    p[0] = (v >> 40) & 0xFF; p[1] = (v >> 32) & 0xFF;
    p[2] = (v >> 24) & 0xFF; p[3] = (v >> 16) & 0xFF;
    p[4] = (v >> 8)  & 0xFF; p[5] =  v & 0xFF;
}
inline void put64(uint8_t* p, uint64_t v) noexcept {
    p[0] = (v >> 56) & 0xFF; p[1] = (v >> 48) & 0xFF;
    p[2] = (v >> 40) & 0xFF; p[3] = (v >> 32) & 0xFF;
    p[4] = (v >> 24) & 0xFF; p[5] = (v >> 16) & 0xFF;
    p[6] = (v >> 8)  & 0xFF; p[7] =  v & 0xFF;
}
inline void put_stock(uint8_t* p, const char* sym) noexcept {
    std::memset(p, ' ', 8);
    std::size_t n = std::strlen(sym);
    if (n > 8) n = 8;
    std::memcpy(p, sym, n);
}

// ── Message builders — return (msg_bytes, total_length_including_prefix) ──

// Prepend the 2-byte length header; writes into caller-provided buffer.
// Returns the total bytes written (2 + msg_len).
inline std::size_t frame(uint8_t* buf, const uint8_t* msg, uint16_t msg_len) noexcept {
    put16(buf, msg_len);
    std::memcpy(buf + 2, msg, msg_len);
    return 2 + msg_len;
}

// Stock Directory message (len=33 after type byte → total payload = 33 bytes)
inline std::size_t stock_directory(uint8_t* buf,
                                   uint16_t locate,
                                   uint64_t ts_ns,
                                   const char* sym) noexcept {
    uint8_t msg[33]{};
    msg[0] = 'R';
    put16(msg + 1, locate);
    put16(msg + 3, 0);         // tracking number
    put48(msg + 5, ts_ns);
    put_stock(msg + 11, sym);
    // remaining fields (market category, etc.) left zero
    return frame(buf, msg, 33);
}

// Add Order — type 'A' (36 bytes payload)
// Layout: type(1)+locate(2)+tracking(2)+ts(6)+ref(8)+side(1)+shares(4)+stock(8)+price(4)=36
inline std::size_t add_order(uint8_t* buf,
                              uint16_t locate,
                              uint64_t ts_ns,
                              uint64_t order_ref,
                              char     buy_sell,
                              uint32_t shares,
                              const char* sym,
                              uint32_t price) noexcept {
    uint8_t msg[36]{};
    msg[0] = 'A';
    put16(msg + 1,  locate);
    put16(msg + 3,  0);
    put48(msg + 5,  ts_ns);
    put64(msg + 11, order_ref);
    msg[19] = static_cast<uint8_t>(buy_sell);
    put32(msg + 20, shares);
    put_stock(msg + 24, sym);
    put32(msg + 32, price);
    return frame(buf, msg, 36);
}

// Order Executed — type 'E' (31 bytes payload)
// Layout: type(1)+locate(2)+tracking(2)+ts(6)+ref(8)+exec_shares(4)+match(8)=31
inline std::size_t order_executed(uint8_t* buf,
                                   uint16_t locate,
                                   uint64_t ts_ns,
                                   uint64_t order_ref,
                                   uint32_t exec_shares,
                                   uint64_t match_num) noexcept {
    uint8_t msg[31]{};
    msg[0] = 'E';
    put16(msg + 1,  locate);
    put16(msg + 3,  0);
    put48(msg + 5,  ts_ns);
    put64(msg + 11, order_ref);
    put32(msg + 19, exec_shares);
    put64(msg + 23, match_num);
    return frame(buf, msg, 31);
}

// Order Cancel — type 'X' (23 bytes payload)
// Layout: type(1)+locate(2)+tracking(2)+ts(6)+ref(8)+cancelled(4)=23
inline std::size_t order_cancel(uint8_t* buf,
                                 uint16_t locate,
                                 uint64_t ts_ns,
                                 uint64_t order_ref,
                                 uint32_t cancelled_shares) noexcept {
    uint8_t msg[23]{};
    msg[0] = 'X';
    put16(msg + 1,  locate);
    put16(msg + 3,  0);
    put48(msg + 5,  ts_ns);
    put64(msg + 11, order_ref);
    put32(msg + 19, cancelled_shares);
    return frame(buf, msg, 23);
}

// Order Delete — type 'D' (19 bytes payload)
// Layout: type(1)+locate(2)+tracking(2)+ts(6)+ref(8)=19
inline std::size_t order_delete(uint8_t* buf,
                                 uint16_t locate,
                                 uint64_t ts_ns,
                                 uint64_t order_ref) noexcept {
    uint8_t msg[19]{};
    msg[0] = 'D';
    put16(msg + 1,  locate);
    put16(msg + 3,  0);
    put48(msg + 5,  ts_ns);
    put64(msg + 11, order_ref);
    return frame(buf, msg, 19);
}

// Order Replace — type 'U' (35 bytes payload)
inline std::size_t order_replace(uint8_t* buf,
                                  uint16_t locate,
                                  uint64_t ts_ns,
                                  uint64_t orig_ref,
                                  uint64_t new_ref,
                                  uint32_t shares,
                                  uint32_t price) noexcept {
    uint8_t msg[35]{};
    msg[0] = 'U';
    put16(msg + 1,  locate);
    put16(msg + 3,  0);
    put48(msg + 5,  ts_ns);
    put64(msg + 11, orig_ref);
    put64(msg + 19, new_ref);
    put32(msg + 27, shares);
    put32(msg + 31, price);
    return frame(buf, msg, 35);
}

} // namespace itch_build
