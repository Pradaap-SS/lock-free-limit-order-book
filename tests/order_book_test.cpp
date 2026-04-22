#include <gtest/gtest.h>
#include "order_book.h"

class OrderBookTest : public ::testing::Test {
protected:
    std::vector<Event> events;
    OrderBook book{[this](const Event& e) { events.push_back(e); }};

    void clear_events() { events.clear(); }

    int count_events(EventType t) {
        return static_cast<int>(std::count_if(events.begin(), events.end(),
            [t](const Event& e) { return e.type == t; }));
    }
};

TEST_F(OrderBookTest, AddPassiveBidUpdatesTopOfBook) {
    EXPECT_TRUE(book.add_order(1, 100, 500, Side::Buy));
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, 100u);
    EXPECT_EQ(tob.bid_qty, 500u);
}

TEST_F(OrderBookTest, AddPassiveAskUpdatesTopOfBook) {
    EXPECT_TRUE(book.add_order(1, 200, 300, Side::Sell));
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.ask_price, 200u);
    EXPECT_EQ(tob.ask_qty, 300u);
}

TEST_F(OrderBookTest, FullMatchClearsLevel) {
    book.add_order(1, 100, 200, Side::Sell);
    clear_events();
    book.add_order(2, 101, 200, Side::Buy); // fully crosses

    EXPECT_EQ(count_events(EventType::Trade), 1);
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.ask_qty, 0u);
}

TEST_F(OrderBookTest, PartialMatchLeavesRemainder) {
    book.add_order(1, 100, 200, Side::Sell);
    clear_events();
    book.add_order(2, 101, 100, Side::Buy); // partial fill

    EXPECT_EQ(count_events(EventType::Trade), 1);
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.ask_price, 100u);
    EXPECT_EQ(tob.ask_qty, 100u); // 100 remaining
}

TEST_F(OrderBookTest, CancelOrderRemovesFromBook) {
    book.add_order(1, 100, 500, Side::Buy);
    EXPECT_TRUE(book.cancel_order(1));
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.bid_qty, 0u);
}

TEST_F(OrderBookTest, CancelNonExistentReturnsFalse) {
    EXPECT_FALSE(book.cancel_order(999));
}

TEST_F(OrderBookTest, PriceTimePriorityOrderedCorrectly) {
    // Two orders at same price: first in should match first
    book.add_order(10, 100, 50, Side::Sell);
    book.add_order(11, 100, 50, Side::Sell);
    clear_events();

    book.add_order(20, 101, 50, Side::Buy);

    ASSERT_EQ(count_events(EventType::Trade), 1);
    auto trade = std::find_if(events.begin(), events.end(),
        [](const Event& e) { return e.type == EventType::Trade; });
    EXPECT_EQ(trade->trade.passive_id, 10u); // order 10 matched first
}

TEST_F(OrderBookTest, BestBidUpdatesAfterCancel) {
    book.add_order(1, 105, 100, Side::Buy);
    book.add_order(2, 100, 100, Side::Buy);
    book.cancel_order(1);
    auto tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, 100u);
}

TEST_F(OrderBookTest, OrdersInFlightCount) {
    book.add_order(1, 100, 100, Side::Buy);
    book.add_order(2, 200, 100, Side::Sell);
    EXPECT_EQ(book.orders_in_flight(), 2u);
    book.cancel_order(1);
    EXPECT_EQ(book.orders_in_flight(), 1u);
}
