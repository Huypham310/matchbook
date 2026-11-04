#include <gtest/gtest.h>

#include <vector>

#include "matchbook/engine.hpp"

using namespace matchbook;

namespace {

Event trade(OrderId taker, OrderId maker, Price px, Qty q) {
    return {EventType::Trade, taker, maker, px, q, {}};
}
Event rested(OrderId id, Price px, Qty q) { return {EventType::Rested, id, 0, px, q, {}}; }
Event cancelled(OrderId id, Price px, Qty q) { return {EventType::Cancelled, id, 0, px, q, {}}; }

NewOrder limit(OrderId id, Side s, Price px, Qty q, TimeInForce tif = TimeInForce::GTC) {
    return {id, s, OrderType::Limit, tif, px, q};
}

// a book with asks 101 x 5 (id 1), 101 x 3 (id 2), 103 x 4 (id 3) and bids 99 x 6 (id 4)
MatchingEngine seeded() {
    MatchingEngine e(64, 64);
    e.submit(limit(1, Side::Sell, 101, 5));
    e.submit(limit(2, Side::Sell, 101, 3));
    e.submit(limit(3, Side::Sell, 103, 4));
    e.submit(limit(4, Side::Buy, 99, 6));
    return e;
}

}  // namespace

TEST(Engine, NonCrossingLimitRests) {
    MatchingEngine e(16, 16);
    e.submit(limit(1, Side::Buy, 100, 5));
    EXPECT_EQ(e.events(), (std::vector<Event>{rested(1, 100, 5)}));
    EXPECT_EQ(e.book().best_bid(), 100);
}

TEST(Engine, TakerFillsAtMakerPricesInTimePriority) {
    auto e = seeded();
    e.submit(limit(10, Side::Buy, 103, 10));
    EXPECT_EQ(e.events(), (std::vector<Event>{trade(10, 1, 101, 5), trade(10, 2, 101, 3),
                                              trade(10, 3, 103, 2)}));
    EXPECT_EQ(e.book().volume_at(Side::Sell, 103), 2);  // maker 3 keeps 2
    EXPECT_FALSE(e.book().volume_at(Side::Sell, 101));
    EXPECT_EQ(e.trade_count(), 3u);
}

TEST(Engine, GtcRemainderRestsAtItsLimit) {
    auto e = seeded();
    e.submit(limit(10, Side::Buy, 102, 10));
    EXPECT_EQ(e.events(), (std::vector<Event>{trade(10, 1, 101, 5), trade(10, 2, 101, 3),
                                              rested(10, 102, 2)}));
    EXPECT_EQ(e.book().best_bid(), 102);
    EXPECT_EQ(e.book().best_ask(), 103);  // never crossed
}

TEST(Engine, IocDropsTheRemainder) {
    auto e = seeded();
    e.submit(limit(10, Side::Buy, 101, 10, TimeInForce::IOC));
    EXPECT_EQ(e.events(), (std::vector<Event>{trade(10, 1, 101, 5), trade(10, 2, 101, 3),
                                              cancelled(10, 101, 2)}));
    EXPECT_EQ(e.book().best_bid(), 99);
}

TEST(Engine, FokIsAllOrNothing) {
    auto e = seeded();
    e.submit(limit(10, Side::Buy, 101, 9, TimeInForce::FOK));  // only 8 available at <= 101
    EXPECT_EQ(e.events(), (std::vector<Event>{cancelled(10, 101, 9)}));
    EXPECT_EQ(e.book().volume_at(Side::Sell, 101), 8);  // untouched

    e.submit(limit(11, Side::Buy, 103, 9, TimeInForce::FOK));
    EXPECT_EQ(e.events(), (std::vector<Event>{trade(11, 1, 101, 5), trade(11, 2, 101, 3),
                                              trade(11, 3, 103, 1)}));
}

TEST(Engine, FokThatExactlyMatchesTheLiquidityFills) {
    auto e = seeded();
    e.submit(limit(10, Side::Buy, 101, 8, TimeInForce::FOK));  // exactly the 8 at 101
    EXPECT_EQ(e.events(), (std::vector<Event>{trade(10, 1, 101, 5), trade(10, 2, 101, 3)}));
    EXPECT_EQ(e.book().best_ask(), 103);
}

TEST(Engine, MarketOrderSweepsAndNeverRests) {
    auto e = seeded();
    e.submit({10, Side::Sell, OrderType::Market, TimeInForce::GTC, 0, 8});
    EXPECT_EQ(e.events(), (std::vector<Event>{trade(10, 4, 99, 6), cancelled(10, 0, 2)}));
    EXPECT_FALSE(e.book().best_bid());

    MatchingEngine empty(8, 8);
    empty.submit({1, Side::Buy, OrderType::Market, TimeInForce::IOC, 0, 3});
    EXPECT_EQ(empty.events(), (std::vector<Event>{cancelled(1, 0, 3)}));
}

TEST(Engine, RejectsBadOrders) {
    auto e = seeded();
    e.submit(limit(1, Side::Buy, 50, 1));  // id 1 is resting
    EXPECT_EQ(e.events()[0].type, EventType::Rejected);
    EXPECT_EQ(e.events()[0].reason, RejectReason::DuplicateId);
    e.submit(limit(20, Side::Buy, 50, 0));
    EXPECT_EQ(e.events()[0].reason, RejectReason::BadQuantity);
    e.cancel(999);
    EXPECT_EQ(e.events()[0].reason, RejectReason::UnknownOrder);
}

TEST(Engine, FilledOrderIdCanBeReused) {
    auto e = seeded();
    e.submit(limit(10, Side::Buy, 101, 5));  // fills id 1 completely
    e.submit(limit(1, Side::Buy, 98, 1));    // id 1 no longer resting
    EXPECT_EQ(e.events(), (std::vector<Event>{rested(1, 98, 1)}));
}

TEST(Engine, CancelReportsWhatWasLeft) {
    auto e = seeded();
    e.submit(limit(10, Side::Buy, 101, 2));  // id 1 left with 3
    e.cancel(1);
    EXPECT_EQ(e.events(), (std::vector<Event>{cancelled(1, 101, 3)}));
}

TEST(Engine, ModifyThroughTheSpreadTrades) {
    auto e = seeded();
    e.modify(4, 101, 6);  // bid 99 -> 101 crosses the 101 asks
    EXPECT_EQ(e.events(), (std::vector<Event>{trade(4, 1, 101, 5), trade(4, 2, 101, 1)}));
    EXPECT_EQ(e.book().volume_at(Side::Sell, 101), 2);

    e.modify(3, 104, 4);  // passive reprice
    EXPECT_EQ(e.events(), (std::vector<Event>{rested(3, 104, 4)}));
    e.book().check_invariants();
}
