#include <gtest/gtest.h>

#include "matchbook/types.hpp"

using matchbook::crosses;
using matchbook::opposite;
using matchbook::Side;

TEST(Side, OppositeAndName) {
    EXPECT_EQ(opposite(Side::Buy), Side::Sell);
    EXPECT_EQ(opposite(Side::Sell), Side::Buy);
    EXPECT_EQ(matchbook::to_string(Side::Buy), "buy");
}

TEST(Crosses, BuyerCrossesAtOrAboveAsk) {
    EXPECT_TRUE(crosses(Side::Buy, 101, 100));
    EXPECT_TRUE(crosses(Side::Buy, 100, 100));
    EXPECT_FALSE(crosses(Side::Buy, 99, 100));
}

TEST(Crosses, SellerCrossesAtOrBelowBid) {
    EXPECT_TRUE(crosses(Side::Sell, 99, 100));
    EXPECT_TRUE(crosses(Side::Sell, 100, 100));
    EXPECT_FALSE(crosses(Side::Sell, 101, 100));
}

// the engine relies on these being trivially copyable and small
static_assert(sizeof(matchbook::Price) == 8);
static_assert(sizeof(Side) == 1);
