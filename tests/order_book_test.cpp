#include <gtest/gtest.h>
#include <vector>
#include <algorithm>
#include "order_book.h"

// ── Test fixture ──────────────────────────────────────────────────────────────
class OrderBookTest : public ::testing::Test {
protected:
    std::vector<Event> events;
    OrderBook book{[this](const Event& e) { events.push_back(e); }};

    void clear_events() { events.clear(); }

    int count_events(EventType t) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(),
            [t](const Event& e) { return e.type == t; }));
    }

    const Event* find_event(EventType t) const {
        auto it = std::find_if(events.begin(), events.end(),
            [t](const Event& e) { return e.type == t; });
        return it != events.end() ? &*it : nullptr;
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// DAY 1 — Core correctness (regression guard)
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(OrderBookTest, AddPassiveBidUpdatesTopOfBook) {
    EXPECT_TRUE(book.add_order(1, 100, 500, Side::Buy));
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, 100u);
    EXPECT_EQ(tob.bid_qty,   500u);
}

TEST_F(OrderBookTest, AddPassiveAskUpdatesTopOfBook) {
    EXPECT_TRUE(book.add_order(1, 200, 300, Side::Sell));
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.ask_price, 200u);
    EXPECT_EQ(tob.ask_qty,   300u);
}

TEST_F(OrderBookTest, FullMatchClearsLevel) {
    book.add_order(1, 100, 200, Side::Sell);
    clear_events();
    book.add_order(2, 101, 200, Side::Buy);

    EXPECT_EQ(count_events(EventType::Trade), 1);
    EXPECT_EQ(book.top_of_book().ask_qty, 0u);
}

TEST_F(OrderBookTest, PartialMatchLeavesRemainder) {
    book.add_order(1, 100, 200, Side::Sell);
    clear_events();
    book.add_order(2, 101, 100, Side::Buy);

    EXPECT_EQ(count_events(EventType::Trade), 1);
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.ask_price, 100u);
    EXPECT_EQ(tob.ask_qty,   100u);
}

TEST_F(OrderBookTest, CancelOrderRemovesFromBook) {
    book.add_order(1, 100, 500, Side::Buy);
    EXPECT_TRUE(book.cancel_order(1));
    EXPECT_EQ(book.top_of_book().bid_qty, 0u);
}

TEST_F(OrderBookTest, CancelNonExistentReturnsFalse) {
    EXPECT_FALSE(book.cancel_order(999));
}

TEST_F(OrderBookTest, PriceTimePriorityOrderedCorrectly) {
    book.add_order(10, 100, 50, Side::Sell);
    book.add_order(11, 100, 50, Side::Sell);
    clear_events();
    book.add_order(20, 101, 50, Side::Buy);

    ASSERT_EQ(count_events(EventType::Trade), 1);
    const Event* t = find_event(EventType::Trade);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->trade.passive_id, 10u); // time priority: #10 before #11
}

TEST_F(OrderBookTest, BestBidUpdatesAfterCancel) {
    book.add_order(1, 105, 100, Side::Buy);
    book.add_order(2, 100, 100, Side::Buy);
    book.cancel_order(1);
    EXPECT_EQ(book.top_of_book().bid_price, 100u);
}

TEST_F(OrderBookTest, OrdersInFlightCount) {
    book.add_order(1, 100, 100, Side::Buy);
    book.add_order(2, 200, 100, Side::Sell);
    EXPECT_EQ(book.orders_in_flight(), 2u);
    book.cancel_order(1);
    EXPECT_EQ(book.orders_in_flight(), 1u);
}

// ══════════════════════════════════════════════════════════════════════════════
// DAY 2 — modify_order
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(OrderBookTest, ModifyOrder_SamePrice_QuantityReduction_InPlace) {
    book.add_order(1, 100, 500, Side::Buy);
    clear_events();

    EXPECT_TRUE(book.modify_order(1, 100, 300));

    // Emits a Modify event (not Cancel + Ack)
    EXPECT_EQ(count_events(EventType::OrderModify), 1);
    EXPECT_EQ(count_events(EventType::OrderCancel), 0);

    // Quantity reflects the new value
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, 100u);
    EXPECT_EQ(tob.bid_qty,   300u);
}

TEST_F(OrderBookTest, ModifyOrder_SamePrice_QuantityIncrease_ReAdds) {
    // Quantity increase at same price loses time priority → cancel + re-add
    book.add_order(1, 100, 200, Side::Buy);
    clear_events();

    EXPECT_TRUE(book.modify_order(1, 100, 500));

    EXPECT_EQ(count_events(EventType::OrderCancel), 1);
    EXPECT_EQ(count_events(EventType::OrderAck),    1);
    EXPECT_EQ(book.top_of_book().bid_qty, 500u);
}

TEST_F(OrderBookTest, ModifyOrder_DifferentPrice_CancelAndReAdd) {
    book.add_order(1, 100, 300, Side::Buy);
    clear_events();

    EXPECT_TRUE(book.modify_order(1, 99, 300));

    EXPECT_EQ(count_events(EventType::OrderCancel), 1);
    EXPECT_EQ(count_events(EventType::OrderAck),    1);

    auto tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, 99u);  // moved to new price
    EXPECT_EQ(tob.bid_qty,   300u);
}

TEST_F(OrderBookTest, ModifyOrder_NonExistent_ReturnsFalse) {
    EXPECT_FALSE(book.modify_order(999, 100, 100));
}

TEST_F(OrderBookTest, ModifyOrder_PreservesTimePriority) {
    // Two orders at price 100; reduce #1's qty in place.
    // #1 must still be first in the queue (no cancel+re-add).
    book.add_order(1, 100, 500, Side::Sell); // first in queue
    book.add_order(2, 100, 500, Side::Sell); // second
    clear_events();

    book.modify_order(1, 100, 200); // in-place reduction

    // Now match — #1 should fill first
    book.add_order(3, 101, 200, Side::Buy);

    ASSERT_EQ(count_events(EventType::Trade), 1);
    const Event* t = find_event(EventType::Trade);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->trade.passive_id, 1u);
}

TEST_F(OrderBookTest, ModifyOrder_UpdatesBookStats) {
    book.add_order(1, 100, 1000, Side::Buy);
    auto before = book.get_stats();
    EXPECT_EQ(before.bid_total_qty, 1000u);

    book.modify_order(1, 100, 600); // reduce by 400

    auto after = book.get_stats();
    EXPECT_EQ(after.bid_total_qty, 600u);
}

// ══════════════════════════════════════════════════════════════════════════════
// DAY 2 — multi-level matching
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(OrderBookTest, MultiLevel_AggressorSweepsTwoAskLevels) {
    // Two ask levels: 200sh at 100, 300sh at 101
    book.add_order(1, 100, 200, Side::Sell);
    book.add_order(2, 101, 300, Side::Sell);
    clear_events();

    // Large buy: sweeps both levels completely
    book.add_order(3, 102, 500, Side::Buy);

    EXPECT_EQ(count_events(EventType::Trade), 2);
    EXPECT_EQ(book.top_of_book().ask_qty, 0u); // both levels consumed
    EXPECT_EQ(book.orders_in_flight(), 0u);    // both passives returned to pool
}

TEST_F(OrderBookTest, MultiLevel_AggressorPartiallyFillsSecondLevel) {
    book.add_order(1, 100, 200, Side::Sell);
    book.add_order(2, 101, 300, Side::Sell);
    clear_events();

    // Buy 350 — fully fills level 1 (200), partially fills level 2 (150 of 300)
    book.add_order(3, 102, 350, Side::Buy);

    EXPECT_EQ(count_events(EventType::Trade), 2);

    auto tob = book.top_of_book();
    EXPECT_EQ(tob.ask_price, 101u);
    EXPECT_EQ(tob.ask_qty,   150u); // 300 - 150 = 150 remaining
}

TEST_F(OrderBookTest, MultiLevel_AggressorSweepsThreeBidLevels) {
    // Sell side: three bid levels
    book.add_order(1, 102, 100, Side::Buy);
    book.add_order(2, 101, 100, Side::Buy);
    book.add_order(3, 100, 100, Side::Buy);
    clear_events();

    book.add_order(4, 99, 300, Side::Sell); // aggressive sell

    EXPECT_EQ(count_events(EventType::Trade), 3);
    EXPECT_EQ(book.top_of_book().bid_qty, 0u);
}

// ══════════════════════════════════════════════════════════════════════════════
// DAY 2 — BookStats and get_depth
// ══════════════════════════════════════════════════════════════════════════════

TEST_F(OrderBookTest, BookStats_SpreadAndMid) {
    book.add_order(1, 100, 500, Side::Buy);
    book.add_order(2, 103, 300, Side::Sell);

    auto s = book.get_stats();
    EXPECT_EQ(s.best_bid, 100u);
    EXPECT_EQ(s.best_ask, 103u);
    EXPECT_EQ(s.spread,     3u); // 103 - 100
    EXPECT_EQ(s.mid,      101u); // (100 + 103) / 2
}

TEST_F(OrderBookTest, BookStats_TotalQuantityTracked) {
    book.add_order(1, 100, 400, Side::Buy);
    book.add_order(2, 100, 200, Side::Buy);  // same level
    book.add_order(3, 101, 600, Side::Sell);

    auto s = book.get_stats();
    EXPECT_EQ(s.bid_total_qty, 600u); // 400 + 200
    EXPECT_EQ(s.ask_total_qty, 600u);

    book.cancel_order(1);
    s = book.get_stats();
    EXPECT_EQ(s.bid_total_qty, 200u); // only order 2 remains
}

TEST_F(OrderBookTest, BookStats_TotalQuantityAfterMatch) {
    book.add_order(1, 100, 500, Side::Sell);
    book.add_order(2, 100, 200, Side::Sell);
    auto before = book.get_stats();
    EXPECT_EQ(before.ask_total_qty, 700u);

    // Aggressive buy fills 300 shares across both orders
    book.add_order(3, 101, 300, Side::Buy);

    auto after = book.get_stats();
    EXPECT_EQ(after.ask_total_qty, 400u); // 700 - 300
}

TEST_F(OrderBookTest, GetDepth_ReturnsSortedLevels) {
    book.add_order(1, 100, 100, Side::Buy);
    book.add_order(2,  99, 200, Side::Buy);
    book.add_order(3,  98, 300, Side::Buy);
    book.add_order(4, 101, 150, Side::Sell);
    book.add_order(5, 102, 250, Side::Sell);

    std::vector<OrderBook::DepthLevel> bids, asks;
    book.get_depth(bids, asks, 3);

    ASSERT_EQ(bids.size(), 3u);
    ASSERT_EQ(asks.size(), 2u);

    // Bids: high → low
    EXPECT_EQ(bids[0].price, 100u);
    EXPECT_EQ(bids[1].price,  99u);
    EXPECT_EQ(bids[2].price,  98u);

    // Asks: low → high
    EXPECT_EQ(asks[0].price, 101u);
    EXPECT_EQ(asks[1].price, 102u);
}
