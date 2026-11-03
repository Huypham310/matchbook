#include <gtest/gtest.h>

#include <vector>

#include "matchbook/book.hpp"
#include "matchbook/pool.hpp"

using namespace matchbook;

namespace {

std::vector<OrderId> queue_at(const OrderBook& book, OrderId any_order_at_level) {
    std::vector<OrderId> ids;
    for (const Order* o = book.find(any_order_at_level)->level->head; o; o = o->next) {
        ids.push_back(o->id);
    }
    return ids;
}

}  // namespace

TEST(Pool, HandsOutEverySlotOnceThenRunsDry) {
    Pool<Order> pool(3);
    Order* a = pool.acquire();
    Order* b = pool.acquire();
    Order* c = pool.acquire();
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_EQ(pool.acquire(), nullptr);
    EXPECT_EQ(pool.in_use(), 3u);
    pool.release(b);
    EXPECT_EQ(pool.acquire(), b);  // reused, no new memory
}

TEST(Pool, ReacquiredSlotIsReset) {
    Pool<Order> pool(1);
    Order* o = pool.acquire();
    o->id = 42;
    o->qty = 7;
    pool.release(o);
    Order* again = pool.acquire();
    EXPECT_EQ(again->id, 0u);
    EXPECT_EQ(again->qty, 0);
}

TEST(Book, EmptyBook) {
    OrderBook book(16, 16);
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
    EXPECT_TRUE(book.depth(Side::Buy, 5).empty());
    book.check_invariants();
}

TEST(Book, BestPricesAndDepthAggregateLevels) {
    OrderBook book(16, 16);
    ASSERT_EQ(book.add(1, Side::Buy, 100, 5), AddResult::Ok);
    ASSERT_EQ(book.add(2, Side::Buy, 101, 3), AddResult::Ok);
    ASSERT_EQ(book.add(3, Side::Buy, 100, 2), AddResult::Ok);
    ASSERT_EQ(book.add(4, Side::Sell, 105, 4), AddResult::Ok);
    ASSERT_EQ(book.add(5, Side::Sell, 103, 1), AddResult::Ok);

    EXPECT_EQ(book.best_bid(), 101);
    EXPECT_EQ(book.best_ask(), 103);
    EXPECT_EQ(book.depth(Side::Buy, 10),
              (std::vector<LevelView>{{101, 3, 1}, {100, 7, 2}}));
    EXPECT_EQ(book.depth(Side::Sell, 1), (std::vector<LevelView>{{103, 1, 1}}));
    EXPECT_EQ(book.volume_at(Side::Buy, 100), 7);
    EXPECT_EQ(book.volume_at(Side::Buy, 99), 0);
    book.check_invariants();
}

TEST(Book, OrdersQueueFifoWithinALevel) {
    OrderBook book(16, 16);
    for (OrderId id : {7, 3, 9}) book.add(id, Side::Sell, 50, 1);
    EXPECT_EQ(queue_at(book, 7), (std::vector<OrderId>{7, 3, 9}));
}

TEST(Book, CancelHeadMiddleAndTail) {
    OrderBook book(16, 16);
    for (OrderId id = 1; id <= 5; ++id) book.add(id, Side::Buy, 10, static_cast<Qty>(id));
    EXPECT_TRUE(book.cancel(3));  // middle
    EXPECT_TRUE(book.cancel(1));  // head
    EXPECT_TRUE(book.cancel(5));  // tail
    EXPECT_EQ(queue_at(book, 2), (std::vector<OrderId>{2, 4}));
    EXPECT_EQ(book.volume_at(Side::Buy, 10), 6);
    EXPECT_FALSE(book.cancel(3));  // already gone
    book.check_invariants();
}

TEST(Book, LastOrderLeavingRemovesTheLevel) {
    OrderBook book(16, 16);
    book.add(1, Side::Buy, 100, 1);
    book.add(2, Side::Buy, 99, 1);
    book.cancel(1);
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.level_count(Side::Buy), 1u);
    book.check_invariants();
}

TEST(Book, ShrinkingKeepsPriorityEverythingElseRequeues) {
    OrderBook book(16, 16);
    for (OrderId id = 1; id <= 3; ++id) book.add(id, Side::Buy, 10, 5);

    EXPECT_TRUE(book.modify(1, 10, 2));  // smaller, same price: stays first
    EXPECT_EQ(queue_at(book, 1), (std::vector<OrderId>{1, 2, 3}));
    EXPECT_EQ(book.volume_at(Side::Buy, 10), 12);

    EXPECT_TRUE(book.modify(1, 10, 9));  // bigger: goes to the back
    EXPECT_EQ(queue_at(book, 1), (std::vector<OrderId>{2, 3, 1}));

    EXPECT_TRUE(book.modify(2, 11, 5));  // new price: new level
    EXPECT_EQ(book.best_bid(), 11);
    EXPECT_TRUE(book.modify(2, 11, 0));  // zero quantity = cancel
    EXPECT_EQ(book.find(2), nullptr);
    book.check_invariants();
}

TEST(Book, RejectsBadInput) {
    OrderBook book(16, 16);
    EXPECT_EQ(book.add(1, Side::Buy, 10, 0), AddResult::BadQuantity);
    EXPECT_EQ(book.add(1, Side::Buy, 10, -3), AddResult::BadQuantity);
    EXPECT_EQ(book.add(1, Side::Buy, 10, 1), AddResult::Ok);
    EXPECT_EQ(book.add(1, Side::Sell, 20, 1), AddResult::DuplicateId);
    EXPECT_FALSE(book.modify(99, 10, 1));
    book.check_invariants();
}

TEST(Book, FullPoolsRejectWithoutCorruptingState) {
    OrderBook book(2, 2);
    EXPECT_EQ(book.add(1, Side::Buy, 10, 1), AddResult::Ok);
    EXPECT_EQ(book.add(2, Side::Buy, 11, 1), AddResult::Ok);
    EXPECT_EQ(book.add(3, Side::Buy, 12, 1), AddResult::BookFull);  // no order slot

    book.cancel(2);
    EXPECT_EQ(book.add(3, Side::Sell, 20, 1), AddResult::Ok);  // slot reused
    EXPECT_EQ(book.add(4, Side::Sell, 21, 1), AddResult::BookFull);
    book.check_invariants();

    // only 2 level slots, both used (bid 10, ask 20): moving to a 3rd price must fail cleanly
    OrderBook tight(4, 2);
    tight.add(1, Side::Buy, 10, 1);
    tight.add(2, Side::Sell, 20, 1);
    tight.add(3, Side::Buy, 10, 1);
    EXPECT_FALSE(tight.modify(3, 12, 1));
    EXPECT_EQ(tight.find(3), nullptr);
    tight.check_invariants();
}
