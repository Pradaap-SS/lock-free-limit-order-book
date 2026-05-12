#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include "itch_parser.h"
#include "replay_engine.h"

// ── Helpers ───────────────────────────────────────────────────────────────────

// Assemble a multi-message ITCH stream from fragments
struct StreamBuilder {
    std::vector<uint8_t> buf;

    void append(const uint8_t* data, std::size_t n) {
        buf.insert(buf.end(), data, data + n);
    }

    void stock_dir(uint16_t locate, const char* sym, uint64_t ts = 0) {
        uint8_t tmp[64];
        append(tmp, itch_build::stock_directory(tmp, locate, ts, sym));
    }

    void add(uint64_t ref, char side, uint32_t qty, const char* sym,
             uint32_t price, uint16_t locate = 1, uint64_t ts = 0) {
        uint8_t tmp[64];
        append(tmp, itch_build::add_order(tmp, locate, ts, ref, side, qty, sym, price));
    }

    void exec(uint64_t ref, uint32_t exec_qty, uint64_t match = 1,
              uint16_t locate = 1, uint64_t ts = 0) {
        uint8_t tmp[64];
        append(tmp, itch_build::order_executed(tmp, locate, ts, ref, exec_qty, match));
    }

    void cancel(uint64_t ref, uint32_t cancelled, uint16_t locate = 1, uint64_t ts = 0) {
        uint8_t tmp[64];
        append(tmp, itch_build::order_cancel(tmp, locate, ts, ref, cancelled));
    }

    void del(uint64_t ref, uint16_t locate = 1, uint64_t ts = 0) {
        uint8_t tmp[64];
        append(tmp, itch_build::order_delete(tmp, locate, ts, ref));
    }

    void replace(uint64_t orig, uint64_t next_ref, uint32_t qty, uint32_t price,
                 uint16_t locate = 1, uint64_t ts = 0) {
        uint8_t tmp[64];
        append(tmp, itch_build::order_replace(tmp, locate, ts, orig, next_ref, qty, price));
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// ITCH Parser unit tests
// ══════════════════════════════════════════════════════════════════════════════

TEST(ItchParserTest, ParseStockDirectory) {
    StreamBuilder b;
    b.stock_dir(42, "AAPL", 123'456'789);

    StockDirectoryMsg got{};
    int calls = 0;

    ItchParser p;
    p.on_stock_directory = [&](const StockDirectoryMsg& m) { got = m; ++calls; };
    p.parse(b.buf.data(), b.buf.size());

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(got.stock_locate, 42u);
    EXPECT_STREQ(got.stock, "AAPL");
    EXPECT_EQ(got.timestamp_ns, 123'456'789ULL);
}

TEST(ItchParserTest, ParseAddOrder) {
    StreamBuilder b;
    // price 1505000 = $150.5000 in ITCH units
    b.add(9001, 'B', 500, "TSLA", 1'505'000, 7, 999'999'000);

    AddOrderMsg got{};
    ItchParser p;
    p.on_add_order = [&](const AddOrderMsg& m) { got = m; };
    p.parse(b.buf.data(), b.buf.size());

    EXPECT_EQ(got.stock_locate, 7u);
    EXPECT_EQ(got.order_ref, 9001u);
    EXPECT_EQ(got.buy_sell, 'B');
    EXPECT_EQ(got.shares, 500u);
    EXPECT_STREQ(got.stock, "TSLA");
    EXPECT_EQ(got.price, 1'505'000u);
    EXPECT_EQ(got.timestamp_ns, 999'999'000ULL);
}

TEST(ItchParserTest, ParseAddOrderSellSide) {
    StreamBuilder b;
    b.add(42, 'S', 100, "MSFT", 3'000'000);

    AddOrderMsg got{};
    ItchParser p;
    p.on_add_order = [&](const AddOrderMsg& m) { got = m; };
    p.parse(b.buf.data(), b.buf.size());

    EXPECT_EQ(got.buy_sell, 'S');
    EXPECT_STREQ(got.stock, "MSFT");
}

TEST(ItchParserTest, ParseOrderExecuted) {
    StreamBuilder b;
    b.exec(7777, 200, 12345678);

    OrderExecutedMsg got{};
    ItchParser p;
    p.on_order_executed = [&](const OrderExecutedMsg& m) { got = m; };
    p.parse(b.buf.data(), b.buf.size());

    EXPECT_EQ(got.order_ref, 7777u);
    EXPECT_EQ(got.executed_shares, 200u);
    EXPECT_EQ(got.match_number, 12345678u);
}

TEST(ItchParserTest, ParseOrderCancel) {
    StreamBuilder b;
    b.cancel(555, 300);

    OrderCancelMsg got{};
    ItchParser p;
    p.on_order_cancel = [&](const OrderCancelMsg& m) { got = m; };
    p.parse(b.buf.data(), b.buf.size());

    EXPECT_EQ(got.order_ref, 555u);
    EXPECT_EQ(got.cancelled_shares, 300u);
}

TEST(ItchParserTest, ParseOrderDelete) {
    StreamBuilder b;
    b.del(888);

    OrderDeleteMsg got{};
    ItchParser p;
    p.on_order_delete = [&](const OrderDeleteMsg& m) { got = m; };
    p.parse(b.buf.data(), b.buf.size());

    EXPECT_EQ(got.order_ref, 888u);
}

TEST(ItchParserTest, ParseOrderReplace) {
    StreamBuilder b;
    b.replace(100, 200, 500, 2'000'000);

    OrderReplaceMsg got{};
    ItchParser p;
    p.on_order_replace = [&](const OrderReplaceMsg& m) { got = m; };
    p.parse(b.buf.data(), b.buf.size());

    EXPECT_EQ(got.orig_order_ref, 100u);
    EXPECT_EQ(got.new_order_ref, 200u);
    EXPECT_EQ(got.shares, 500u);
    EXPECT_EQ(got.price, 2'000'000u);
}

TEST(ItchParserTest, ParseMultipleMessages) {
    StreamBuilder b;
    b.stock_dir(1, "AAPL");
    b.add(1, 'B', 100, "AAPL", 1'500'000);
    b.add(2, 'S', 200, "AAPL", 1'510'000);
    b.exec(1, 50, 1);
    b.del(2);

    int dirs = 0, adds = 0, execs = 0, dels = 0;
    ItchParser p;
    p.on_stock_directory = [&](auto&) { ++dirs; };
    p.on_add_order       = [&](auto&) { ++adds; };
    p.on_order_executed  = [&](auto&) { ++execs; };
    p.on_order_delete    = [&](auto&) { ++dels; };

    uint64_t total = p.parse(b.buf.data(), b.buf.size());

    EXPECT_EQ(total, 5u);
    EXPECT_EQ(dirs,  1);
    EXPECT_EQ(adds,  2);
    EXPECT_EQ(execs, 1);
    EXPECT_EQ(dels,  1);
}

TEST(ItchParserTest, NullCallbackSkipsMessage) {
    StreamBuilder b;
    b.add(1, 'B', 100, "AAPL", 1'500'000);

    // Leave on_add_order as null — must not crash
    ItchParser p;
    uint64_t n = p.parse(b.buf.data(), b.buf.size());
    EXPECT_EQ(n, 1u); // message parsed but callback not called
}

TEST(ItchParserTest, TruncatedMessageIsSafelyIgnored) {
    // Write a valid 2-byte length header claiming 35 bytes but only provide 10
    std::vector<uint8_t> bad(12);
    bad[0] = 0; bad[1] = 35; // says 35 bytes follow
    bad[2] = 'A';            // message type byte only

    ItchParser p;
    int calls = 0;
    p.on_add_order = [&](auto&) { ++calls; };

    // Should not crash — truncated message is skipped
    uint64_t n = p.parse(bad.data(), bad.size());
    EXPECT_EQ(n, 0u);    // no complete message dispatched
    EXPECT_EQ(calls, 0); // callback never fired
}

TEST(ItchParserTest, EmptyBufferIsHandled) {
    ItchParser p;
    uint64_t n = p.parse(nullptr, 0);
    EXPECT_EQ(n, 0u);
}

TEST(ItchParserTest, BigEndianFieldsDecodedCorrectly) {
    // Manually construct an Add Order message and verify every field
    // $999.9999 = 9,999,999 in ITCH units
    // order_ref = 0xDEADBEEFCAFEBABE
    StreamBuilder b;
    b.add(0xDEADBEEFCAFEBABEULL, 'S', 0x01020304u, "TEST", 9'999'999u);

    AddOrderMsg got{};
    ItchParser p;
    p.on_add_order = [&](const AddOrderMsg& m) { got = m; };
    p.parse(b.buf.data(), b.buf.size());

    EXPECT_EQ(got.order_ref, 0xDEADBEEFCAFEBABEULL);
    EXPECT_EQ(got.shares, 0x01020304u);
    EXPECT_EQ(got.price, 9'999'999u);
    EXPECT_STREQ(got.stock, "TEST");
}

// ══════════════════════════════════════════════════════════════════════════════
// ReplayEngine integration tests
// ══════════════════════════════════════════════════════════════════════════════

// Helper: build a minimal ITCH stream and run the replay engine
static ReplayEngine::Config aapl_cfg() {
    ReplayEngine::Config cfg;
    cfg.symbol       = "AAPL";
    cfg.tick_divisor = 100; // ITCH price / 100 = cents
    return cfg;
}

TEST(ReplayEngineTest, AddPassiveOrderAppearsInBook) {
    StreamBuilder b;
    b.stock_dir(1, "AAPL");
    b.add(1, 'B', 500, "AAPL", 1'500'000); // $150.00 → tick 15000

    ReplayEngine engine;
    engine.run(b.buf.data(), b.buf.size(), aapl_cfg());

    auto tob = engine.book().top_of_book();
    EXPECT_EQ(tob.bid_price, 15000u); // 1500000 / 100
    EXPECT_EQ(tob.bid_qty, 500u);
}

TEST(ReplayEngineTest, DeleteOrderRemovesFromBook) {
    StreamBuilder b;
    b.stock_dir(1, "AAPL");
    b.add(1, 'B', 500, "AAPL", 1'500'000);
    b.del(1);

    ReplayEngine engine;
    engine.run(b.buf.data(), b.buf.size(), aapl_cfg());

    EXPECT_EQ(engine.book().top_of_book().bid_qty, 0u);
}

TEST(ReplayEngineTest, FullExecutionRemovesOrder) {
    StreamBuilder b;
    b.stock_dir(1, "AAPL");
    b.add(1, 'S', 200, "AAPL", 1'510'000); // ask: 200 @ $151.00
    b.exec(1, 200, 999); // fully executed

    ReplayEngine engine;
    engine.run(b.buf.data(), b.buf.size(), aapl_cfg());

    EXPECT_EQ(engine.book().top_of_book().ask_qty, 0u);
    EXPECT_EQ(engine.book().orders_in_flight(), 0u);
}

TEST(ReplayEngineTest, PartialExecutionReducesQuantity) {
    StreamBuilder b;
    b.stock_dir(1, "AAPL");
    b.add(1, 'S', 300, "AAPL", 1'510'000);
    b.exec(1, 100, 999); // partial: 100 of 300

    ReplayEngine engine;
    engine.run(b.buf.data(), b.buf.size(), aapl_cfg());

    auto tob = engine.book().top_of_book();
    EXPECT_EQ(tob.ask_qty, 200u);      // 300 - 100 = 200 remaining
    EXPECT_EQ(engine.book().orders_in_flight(), 1u); // still in book
}

TEST(ReplayEngineTest, PartialCancelReducesQuantity) {
    StreamBuilder b;
    b.stock_dir(1, "AAPL");
    b.add(1, 'B', 400, "AAPL", 1'500'000);
    b.cancel(1, 150); // cancel 150 of 400

    ReplayEngine engine;
    engine.run(b.buf.data(), b.buf.size(), aapl_cfg());

    EXPECT_EQ(engine.book().top_of_book().bid_qty, 250u); // 400 - 150
}

TEST(ReplayEngineTest, ReplaceOrderUsesNewRefAndPrice) {
    StreamBuilder b;
    b.stock_dir(1, "AAPL");
    b.add(10, 'B', 300, "AAPL", 1'500'000); // $150.00
    b.replace(10, 20, 300, 1'505'000);        // new ref=20, new price $150.05

    ReplayEngine engine;
    engine.run(b.buf.data(), b.buf.size(), aapl_cfg());

    auto tob = engine.book().top_of_book();
    EXPECT_EQ(tob.bid_price, 15050u); // 1505000 / 100 = 15050 cents = $150.50 → wait

    // $150.05 = 1505000/10000 = $150.05. In cents: 15005 cents
    // But our tick_divisor=100: tick = 1505000/100 = 15050. Hmm.
    // 1505000 ITCH units / 100 = 15050 ticks (cents). $150.50? No.
    // ITCH: 1505000 = $150.5000. In cents = $150.50. tick = 15050. That's correct.
    // Wait: $150.05 in ITCH units = 1500500. Let's check what the test sets.
    // b.replace(..., 1505000) → 1505000/10000 = $150.50. In cents: 15050.
    EXPECT_EQ(tob.bid_qty, 300u);
}

TEST(ReplayEngineTest, WrongSymbolIsFiltered) {
    StreamBuilder b;
    b.stock_dir(1, "AAPL");
    b.stock_dir(2, "MSFT");
    b.add(1, 'B', 500, "AAPL", 1'500'000, 1); // AAPL: should be processed
    b.add(2, 'B', 500, "MSFT", 3'000'000, 2); // MSFT: should be filtered

    ReplayEngine engine;
    engine.run(b.buf.data(), b.buf.size(), aapl_cfg());

    // Only AAPL order in book — MSFT filtered by locate
    EXPECT_EQ(engine.book().orders_in_flight(), 1u);
    EXPECT_EQ(engine.book().top_of_book().bid_qty, 500u);
}

TEST(ReplayEngineTest, StatsCountersAreAccurate) {
    StreamBuilder b;
    b.stock_dir(1, "AAPL");
    b.add(1, 'B', 200, "AAPL", 1'500'000);
    b.add(2, 'S', 100, "AAPL", 1'510'000);
    b.exec(2, 100, 1); // full fill
    b.del(1);          // cancel bid

    ReplayEngine engine;
    auto snap = engine.run(b.buf.data(), b.buf.size(), aapl_cfg());

    EXPECT_EQ(snap.orders_added,     2u);
    EXPECT_EQ(snap.orders_cancelled, 2u); // exec full fill + del
    EXPECT_EQ(snap.trades,           1u);
    EXPECT_EQ(snap.shares_traded,  100u);
}

TEST(ReplayEngineTest, MultiLevelMatchOnReplay) {
    // Build a book with 3 ask levels, then inject a large aggressive buy
    // via execution messages
    StreamBuilder b;
    b.stock_dir(1, "AAPL");
    b.add(1, 'S', 100, "AAPL", 1'500'100, 1); // ask at $150.01
    b.add(2, 'S', 200, "AAPL", 1'500'200, 1); // ask at $150.02
    b.add(3, 'S', 300, "AAPL", 1'500'300, 1); // ask at $150.03
    // Aggressive buy executed against all three levels
    b.exec(1, 100, 1); // fills level 1 completely
    b.exec(2, 200, 2); // fills level 2 completely
    b.exec(3, 150, 3); // partially fills level 3

    ReplayEngine engine;
    auto snap = engine.run(b.buf.data(), b.buf.size(), aapl_cfg());

    auto tob = engine.book().top_of_book();
    EXPECT_EQ(tob.ask_qty, 150u);       // 300 - 150 = 150 remaining at level 3
    EXPECT_EQ(snap.trades, 3u);         // 3 fill events
    EXPECT_EQ(snap.shares_traded, 450u); // 100 + 200 + 150
}
