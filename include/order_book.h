#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <functional>
#include <memory>

#include "order.h"
#include "price_level.h"
#include "memory_pool.h"
#include "order_map.h"
#include "spsc_queue.h"
#include "timing.h"

// ── Constants ────────────────────────────────────────────────────────────────
static constexpr std::size_t MAX_PRICE_LEVELS = 100'000; // ticks
// Pool and map live on the heap — each is tens of MB, far exceeding macOS
// default 8 MB thread stack. unique_ptr ensures one-time allocation at
// construction; pointers remain stable for the lifetime of the book.
static constexpr std::size_t ORDER_POOL_SIZE  = 1'048'576; // 64 MB heap
static constexpr std::size_t ORDER_MAP_SIZE   = 1'048'576; // must be power of 2
static constexpr std::size_t EVENT_QUEUE_SIZE = 65'536;

// ── OrderBook ────────────────────────────────────────────────────────────────
class OrderBook {
public:
    using EventCallback = std::function<void(const Event&)>;

    explicit OrderBook(EventCallback cb = nullptr)
        : pool_(std::make_unique<OrderPool<ORDER_POOL_SIZE>>())
        , order_map_(std::make_unique<OrderMap<ORDER_MAP_SIZE>>())
        , event_queue_(std::make_unique<SPSCQueue<Event, EVENT_QUEUE_SIZE>>())
        , event_cb_(std::move(cb))
        , bids_(std::make_unique<std::array<PriceLevel, MAX_PRICE_LEVELS>>())
        , asks_(std::make_unique<std::array<PriceLevel, MAX_PRICE_LEVELS>>())
    {}

    // Returns false if pool is exhausted or price out of range
    bool add_order(OrderId id, Price price, Quantity qty, Side side) noexcept;

    // Returns false if order not found
    bool cancel_order(OrderId id) noexcept;

    // ── Queries ──────────────────────────────────────────────────────────────
    struct TopOfBook {
        Price    bid_price{0}, ask_price{0};
        Quantity bid_qty{0},   ask_qty{0};
    };
    [[nodiscard]] TopOfBook top_of_book() const noexcept;
    [[nodiscard]] std::size_t orders_in_flight() const noexcept;

private:
    void match(Order* aggressor) noexcept;
    void publish(const Event& e) noexcept;
    void update_best_bid() noexcept;
    void update_best_ask() noexcept;

    // Heap-allocated to avoid blowing the 8 MB macOS default stack.
    // All accesses are through raw pointers for zero-overhead dereferencing
    // on the hot path.
    std::unique_ptr<OrderPool<ORDER_POOL_SIZE>>          pool_;
    std::unique_ptr<OrderMap<ORDER_MAP_SIZE>>            order_map_;
    std::unique_ptr<SPSCQueue<Event, EVENT_QUEUE_SIZE>>  event_queue_;
    EventCallback                                        event_cb_;

    // Price levels: index = price tick, bids and asks separated (heap)
    std::unique_ptr<std::array<PriceLevel, MAX_PRICE_LEVELS>> bids_;
    std::unique_ptr<std::array<PriceLevel, MAX_PRICE_LEVELS>> asks_;

    // Cached best bid/ask for O(1) top-of-book query
    Price best_bid_{0};
    Price best_ask_{MAX_PRICE_LEVELS - 1};
};

// ── Inline implementation ────────────────────────────────────────────────────

inline bool OrderBook::add_order(OrderId id, Price price, Quantity qty, Side side) noexcept {
    if (__builtin_expect(price >= MAX_PRICE_LEVELS, 0)) return false;

    Order* o = pool_->acquire();
    if (__builtin_expect(o == nullptr, 0)) return false;

    o->id        = id;
    o->price     = price;
    o->quantity  = qty;
    o->remaining = qty;
    o->side      = side;
    o->timestamp = read_cycles();

    if (!order_map_->insert(id, o)) {
        pool_->release(o);
        return false;
    }

    if (side == Side::Buy) {
        if (__builtin_expect(price >= best_ask_ && !(*asks_)[best_ask_].empty(), 1)) {
            match(o);
        }
        if (!o->is_filled()) {
            (*bids_)[price].push_back(o);
            if (price > best_bid_) best_bid_ = price;
        } else {
            order_map_->erase(id);
            pool_->release(o);
        }
    } else {
        if (__builtin_expect(price <= best_bid_ && !(*bids_)[best_bid_].empty(), 1)) {
            match(o);
        }
        if (!o->is_filled()) {
            (*asks_)[price].push_back(o);
            if (price < best_ask_) best_ask_ = price;
        } else {
            order_map_->erase(id);
            pool_->release(o);
        }
    }

    Event ack{.type = EventType::OrderAck, .order_id = id};
    publish(ack);
    return true;
}

inline void OrderBook::match(Order* aggressor) noexcept {
    // Raw pointer to the passive side's level array — no unique_ptr overhead on hot path
    auto* passive_levels = (aggressor->side == Side::Buy) ? asks_.get() : bids_.get();
    Price& best          = (aggressor->side == Side::Buy) ? best_ask_ : best_bid_;

    while (aggressor->remaining > 0) {
        if (aggressor->side == Side::Buy) {
            while (best < MAX_PRICE_LEVELS && (*passive_levels)[best].empty()) ++best;
            if (best >= MAX_PRICE_LEVELS || aggressor->price < best) break;
        } else {
            while (best > 0 && (*passive_levels)[best].empty()) --best;
            if (best == 0 && (*passive_levels)[best].empty()) break;
            if (aggressor->price > best) break;
        }

        PriceLevel& level = (*passive_levels)[best];
        __builtin_prefetch(level.head, 0, 3);

        while (aggressor->remaining > 0 && !level.empty()) {
            Order* passive = level.head;
            Quantity fill  = std::min(aggressor->remaining, passive->remaining);

            aggressor->remaining -= fill;
            passive->remaining   -= fill;
            level.total_quantity -= fill;

            Event trade{.type = EventType::Trade};
            trade.trade = {aggressor->id, passive->id, passive->price, fill, read_cycles()};
            publish(trade);

            if (passive->is_filled()) {
                level.remove(passive);
                order_map_->erase(passive->id);
                pool_->release(passive);
            }
        }
    }
}

inline bool OrderBook::cancel_order(OrderId id) noexcept {
    Order* o = order_map_->find(id);
    if (__builtin_expect(o == nullptr, 0)) return false;

    auto& levels = (o->side == Side::Buy) ? *bids_ : *asks_;
    levels[o->price].remove(o);
    order_map_->erase(id);
    pool_->release(o);

    if (o->side == Side::Buy && o->price == best_bid_)  update_best_bid();
    if (o->side == Side::Sell && o->price == best_ask_) update_best_ask();

    Event cancel{.type = EventType::OrderCancel, .order_id = id};
    publish(cancel);
    return true;
}

inline void OrderBook::update_best_bid() noexcept {
    while (best_bid_ > 0 && (*bids_)[best_bid_].empty()) --best_bid_;
}

inline void OrderBook::update_best_ask() noexcept {
    while (best_ask_ < MAX_PRICE_LEVELS - 1 && (*asks_)[best_ask_].empty()) ++best_ask_;
}

inline OrderBook::TopOfBook OrderBook::top_of_book() const noexcept {
    TopOfBook tob;
    if (!(*bids_)[best_bid_].empty()) {
        tob.bid_price = best_bid_;
        tob.bid_qty   = (*bids_)[best_bid_].total_quantity;
    }
    if (!(*asks_)[best_ask_].empty()) {
        tob.ask_price = best_ask_;
        tob.ask_qty   = (*asks_)[best_ask_].total_quantity;
    }
    return tob;
}

inline void OrderBook::publish(const Event& e) noexcept {
    if (event_cb_) event_cb_(e);
    event_queue_->push(e);
}

inline std::size_t OrderBook::orders_in_flight() const noexcept {
    return pool_->in_use();
}
