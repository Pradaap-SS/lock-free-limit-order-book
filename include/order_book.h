#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "order.h"
#include "price_level.h"
#include "memory_pool.h"
#include "order_map.h"
#include "spsc_queue.h"
#include "timing.h"
#include "latency_tracer.h"
#include "signpost.h"

// ── Constants ────────────────────────────────────────────────────────────────
static constexpr std::size_t MAX_PRICE_LEVELS = 100'000;
// Pool and map live on the heap — each is tens of MB, far exceeding the
// macOS default 8 MB thread stack.
static constexpr std::size_t ORDER_POOL_SIZE  = 1'048'576;
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
        , sp_log_(signpost::make_log("com.orderbook", "engine"))
    {}

    // ── Core operations ───────────────────────────────────────────────────────

    // Add a new order. Returns false if pool exhausted or price out of range.
    bool add_order(OrderId id, Price price, Quantity qty, Side side) noexcept;

    // Cancel a resting order. Returns false if not found.
    bool cancel_order(OrderId id) noexcept;

    // Modify a resting order.
    //   Same price + qty reduction → in-place O(1), preserves time priority.
    //   Any other change           → cancel + re-add (loses time priority).
    // Returns false if order not found or pool exhausted (re-add path).
    bool modify_order(OrderId id, Price new_price, Quantity new_qty) noexcept;

    // ── Queries ───────────────────────────────────────────────────────────────

    struct TopOfBook {
        Price    bid_price{0}, ask_price{0};
        Quantity bid_qty{0},   ask_qty{0};
    };

    // Aggregated statistics — all O(1).
    struct BookStats {
        Price    best_bid{0};
        Price    best_ask{0};
        uint32_t spread{0};        // best_ask - best_bid (0 if one side empty)
        Price    mid{0};           // (best_bid + best_ask) / 2
        uint64_t bid_total_qty{0}; // total resting shares on bid side
        uint64_t ask_total_qty{0}; // total resting shares on ask side
        std::size_t orders_in_flight{0};
    };

    // Single resting price level for depth snapshots.
    struct DepthLevel {
        Price    price{0};
        Quantity total_qty{0};
        uint32_t order_count{0};
    };

    [[nodiscard]] TopOfBook  top_of_book() const noexcept;
    [[nodiscard]] BookStats  get_stats()   const noexcept;

    // Fill up to n bid levels (high→low) and n ask levels (low→high).
    void get_depth(std::vector<DepthLevel>& bids_out,
                   std::vector<DepthLevel>& asks_out,
                   std::size_t n = 5) const noexcept;

    [[nodiscard]] std::size_t orders_in_flight() const noexcept;

    // ── Diagnostics ───────────────────────────────────────────────────────────
    [[nodiscard]] LatencyTracer&       tracer()       noexcept { return tracer_; }
    [[nodiscard]] const LatencyTracer& tracer() const noexcept { return tracer_; }

private:
    void match(Order* aggressor) noexcept;
    void publish(const Event& e) noexcept;
    void update_best_bid() noexcept;
    void update_best_ask() noexcept;

    // Heap-allocated to stay well within macOS's 8 MB default stack limit.
    std::unique_ptr<OrderPool<ORDER_POOL_SIZE>>          pool_;
    std::unique_ptr<OrderMap<ORDER_MAP_SIZE>>            order_map_;
    std::unique_ptr<SPSCQueue<Event, EVENT_QUEUE_SIZE>>  event_queue_;
    EventCallback                                        event_cb_;
    std::unique_ptr<std::array<PriceLevel, MAX_PRICE_LEVELS>> bids_;
    std::unique_ptr<std::array<PriceLevel, MAX_PRICE_LEVELS>> asks_;

    // Cached best prices for O(1) top-of-book
    Price best_bid_{0};
    Price best_ask_{MAX_PRICE_LEVELS - 1};

    // Incrementally maintained totals — O(1) stats with no scanning
    uint64_t bid_total_qty_{0};
    uint64_t ask_total_qty_{0};

    // Per-stage tracer (zero-cost when ENABLE_TRACING is not defined)
    LatencyTracer tracer_;

    // Instruments signpost log
    decltype(signpost::make_log("", "")) sp_log_;
};

// ── Inline implementation ────────────────────────────────────────────────────

inline bool OrderBook::add_order(OrderId id, Price price,
                                  Quantity qty, Side side) noexcept {
    SIGNPOST_BEGIN(sp_log_, "add_order");

    if (__builtin_expect(price >= MAX_PRICE_LEVELS, 0)) {
        SIGNPOST_END(sp_log_, "add_order");
        return false;
    }

    // ── Stage: pool acquire ──────────────────────────────────────────────────
    Order* o;
    {
        STAGE_TIME(tracer_, Stage::PoolAcquire);
        o = pool_->acquire();
    }
    if (__builtin_expect(o == nullptr, 0)) {
        SIGNPOST_END(sp_log_, "add_order");
        return false;
    }

    o->id        = id;
    o->price     = price;
    o->quantity  = qty;
    o->remaining = qty;
    o->side      = side;
    o->timestamp = read_cycles();

    // ── Stage: hash map insert ───────────────────────────────────────────────
    {
        STAGE_TIME(tracer_, Stage::MapInsert);
        if (!order_map_->insert(id, o)) {
            pool_->release(o);
            SIGNPOST_END(sp_log_, "add_order");
            return false;
        }
    }

    // ── Stage: crossing check + match ────────────────────────────────────────
    {
        STAGE_TIME(tracer_, Stage::CrossCheck);
        if (side == Side::Buy) {
            if (__builtin_expect(
                    price >= best_ask_ && !(*asks_)[best_ask_].empty(), 0)) {
                STAGE_TIME(tracer_, Stage::MatchExecute);
                match(o);
            }
        } else {
            if (__builtin_expect(
                    price <= best_bid_ && !(*bids_)[best_bid_].empty(), 0)) {
                STAGE_TIME(tracer_, Stage::MatchExecute);
                match(o);
            }
        }
    }

    // ── Rest unfilled remainder ───────────────────────────────────────────────
    if (!o->is_filled()) {
        STAGE_TIME(tracer_, Stage::LevelInsert);
        if (side == Side::Buy) {
            (*bids_)[price].push_back(o);
            bid_total_qty_ += o->remaining;
            if (price > best_bid_) best_bid_ = price;
        } else {
            (*asks_)[price].push_back(o);
            ask_total_qty_ += o->remaining;
            if (price < best_ask_) best_ask_ = price;
        }
    } else {
        // Fully matched — reclaim without putting in a price level
        order_map_->erase(id);
        pool_->release(o);
    }

    Event ack{.type = EventType::OrderAck, .order_id = id};
    publish(ack);
    SIGNPOST_END(sp_log_, "add_order");
    return true;
}

inline bool OrderBook::cancel_order(OrderId id) noexcept {
    SIGNPOST_BEGIN(sp_log_, "cancel_order");

    STAGE_TIME(tracer_, Stage::CancelLookup);

    Order* o = order_map_->find(id);
    if (__builtin_expect(o == nullptr, 0)) {
        SIGNPOST_END(sp_log_, "cancel_order");
        return false;
    }

    // Subtract from side total before removing (remaining is still valid here)
    if (o->side == Side::Buy) bid_total_qty_ -= o->remaining;
    else                       ask_total_qty_ -= o->remaining;

    auto& levels = (o->side == Side::Buy) ? *bids_ : *asks_;
    levels[o->price].remove(o);
    order_map_->erase(id);
    pool_->release(o);

    if (o->side == Side::Buy  && o->price == best_bid_)  update_best_bid();
    if (o->side == Side::Sell && o->price == best_ask_) update_best_ask();

    Event cancel{.type = EventType::OrderCancel, .order_id = id};
    publish(cancel);
    SIGNPOST_END(sp_log_, "cancel_order");
    return true;
}

inline bool OrderBook::modify_order(OrderId id, Price new_price,
                                     Quantity new_qty) noexcept {
    Order* o = order_map_->find(id);
    if (__builtin_expect(o == nullptr, 0)) return false;

    // ── Fast path: same price, quantity reduction ─────────────────────────────
    // Keeps the order's position in the time-priority queue intact.
    if (new_price == o->price && new_qty < o->remaining) {
        STAGE_TIME(tracer_, Stage::ModifyInPlace);
        const Quantity delta = o->remaining - new_qty;
        auto& level = (o->side == Side::Buy) ? (*bids_)[o->price]
                                              : (*asks_)[o->price];
        level.total_quantity -= delta;
        if (o->side == Side::Buy) bid_total_qty_ -= delta;
        else                       ask_total_qty_ -= delta;
        o->remaining = new_qty;
        o->quantity  = new_qty;
        Event mod{.type = EventType::OrderModify, .order_id = id};
        publish(mod);
        return true;
    }

    // ── Slow path: price change or quantity increase ───────────────────────────
    // Must cancel + re-add. Order loses time priority (exchange standard).
    const Side side = o->side;
    cancel_order(id);            // emits Cancel event, returns order to pool
    return add_order(id, new_price, new_qty, side); // emits Ack event
}

inline void OrderBook::match(Order* aggressor) noexcept {
    auto* passive_levels =
        (aggressor->side == Side::Buy) ? asks_.get() : bids_.get();
    uint64_t& passive_total =
        (aggressor->side == Side::Buy) ? ask_total_qty_ : bid_total_qty_;
    Price& best =
        (aggressor->side == Side::Buy) ? best_ask_ : best_bid_;

    while (aggressor->remaining > 0) {
        // Advance best over empty levels
        if (aggressor->side == Side::Buy) {
            while (best < MAX_PRICE_LEVELS && (*passive_levels)[best].empty())
                ++best;
            if (best >= MAX_PRICE_LEVELS || aggressor->price < best) break;
        } else {
            while (best > 0 && (*passive_levels)[best].empty()) --best;
            if ((*passive_levels)[best].empty()) break;
            if (aggressor->price > best) break;
        }

        PriceLevel& level = (*passive_levels)[best];

        // Prefetch the head order AND the next price level into cache
        __builtin_prefetch(level.head, 0, 3);
        if (aggressor->side == Side::Buy && best + 1 < MAX_PRICE_LEVELS)
            __builtin_prefetch(&(*passive_levels)[best + 1], 0, 1);
        else if (aggressor->side == Side::Sell && best > 0)
            __builtin_prefetch(&(*passive_levels)[best - 1], 0, 1);

        while (aggressor->remaining > 0 && !level.empty()) {
            Order* passive = level.head;

            // Prefetch the *next* order while we process the current one
            if (passive->next) __builtin_prefetch(passive->next, 0, 3);

            const Quantity fill =
                std::min(aggressor->remaining, passive->remaining);

            aggressor->remaining -= fill;
            passive->remaining   -= fill;
            level.total_quantity -= fill;
            passive_total        -= fill;

            Event trade{.type = EventType::Trade};
            trade.trade = {aggressor->id, passive->id,
                           passive->price, fill, read_cycles()};
            publish(trade);

            if (passive->is_filled()) {
                level.remove(passive);
                order_map_->erase(passive->id);
                pool_->release(passive);
            }
        }
    }
}

inline void OrderBook::update_best_bid() noexcept {
    while (best_bid_ > 0 && (*bids_)[best_bid_].empty()) --best_bid_;
}

inline void OrderBook::update_best_ask() noexcept {
    while (best_ask_ < MAX_PRICE_LEVELS - 1 && (*asks_)[best_ask_].empty())
        ++best_ask_;
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

inline OrderBook::BookStats OrderBook::get_stats() const noexcept {
    BookStats s;
    const bool have_bid = !(*bids_)[best_bid_].empty();
    const bool have_ask = !(*asks_)[best_ask_].empty();
    s.best_bid       = have_bid ? best_bid_ : 0;
    s.best_ask       = have_ask ? best_ask_ : 0;
    s.spread         = (have_bid && have_ask) ? best_ask_ - best_bid_ : 0;
    s.mid            = (have_bid && have_ask) ? (best_bid_ + best_ask_) / 2 : 0;
    s.bid_total_qty  = bid_total_qty_;
    s.ask_total_qty  = ask_total_qty_;
    s.orders_in_flight = pool_->in_use();
    return s;
}

inline void OrderBook::get_depth(std::vector<DepthLevel>& bids_out,
                                  std::vector<DepthLevel>& asks_out,
                                  std::size_t n) const noexcept {
    bids_out.clear();
    asks_out.clear();
    bids_out.reserve(n);
    asks_out.reserve(n);

    // Walk bid side downward from best bid
    for (Price p = best_bid_; p > 0 && bids_out.size() < n; --p) {
        const auto& lvl = (*bids_)[p];
        if (!lvl.empty())
            bids_out.push_back({p, lvl.total_quantity, lvl.count});
    }

    // Walk ask side upward from best ask
    for (Price p = best_ask_; p < MAX_PRICE_LEVELS && asks_out.size() < n; ++p) {
        const auto& lvl = (*asks_)[p];
        if (!lvl.empty())
            asks_out.push_back({p, lvl.total_quantity, lvl.count});
    }
}

inline void OrderBook::publish(const Event& e) noexcept {
    if (event_cb_) event_cb_(e);
    event_queue_->push(e);
}

inline std::size_t OrderBook::orders_in_flight() const noexcept {
    return pool_->in_use();
}
