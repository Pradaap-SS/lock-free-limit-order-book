#pragma once
#include <cstdint>
#include <atomic>

using OrderId  = uint64_t;
using Price    = uint32_t;  // integer ticks (e.g. price * 100)
using Quantity = uint32_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

// Cache-line aligned to 64 bytes — prevents false sharing between adjacent
// orders in the pool and keeps hot fields on one line during matching.
struct alignas(64) Order {
    OrderId  id;
    Price    price;
    Quantity quantity;
    Quantity remaining;
    uint64_t timestamp;  // TSC tick at submission
    Side     side;
    uint8_t  _pad[3];

    // Intrusive doubly-linked list pointers (within a price level)
    Order* next{nullptr};
    Order* prev{nullptr};

    [[nodiscard]] bool is_filled() const noexcept { return remaining == 0; }
};
static_assert(sizeof(Order) == 64, "Order must fit in one cache line");

enum class EventType : uint8_t {
    OrderAck,
    OrderCancel,
    OrderModify,   // in-place quantity reduction at same price (preserves time priority)
    Trade,
    TopOfBookUpdate,
};

struct TradeEvent {
    OrderId  aggressor_id;
    OrderId  passive_id;
    Price    price;
    Quantity quantity;
    uint64_t timestamp;
};

struct TopOfBookEvent {
    Price    bid_price;
    Quantity bid_qty;
    Price    ask_price;
    Quantity ask_qty;
};

struct Event {
    EventType type;
    union {
        OrderId       order_id;  // Ack / Cancel
        TradeEvent    trade;
        TopOfBookEvent tob;
    };
};
