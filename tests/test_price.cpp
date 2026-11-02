#include <gtest/gtest.h>

#include <limits>

#include "matchbook/price.hpp"

using matchbook::format_price;
using matchbook::parse_price;
using matchbook::TickSize;

namespace {
constexpr TickSize kCents{2};
}

TEST(ParsePrice, WholeAndFractional) {
    EXPECT_EQ(parse_price("64321.50", kCents), 6432150);
    EXPECT_EQ(parse_price("64321.5", kCents), 6432150);  // short fraction is scaled up
    EXPECT_EQ(parse_price("64321", kCents), 6432100);
    EXPECT_EQ(parse_price("0.01", kCents), 1);
    EXPECT_EQ(parse_price(".5", kCents), 50);
}

TEST(ParsePrice, TrailingZerosBeyondTickAreFine) {
    EXPECT_EQ(parse_price("64321.500000", kCents), 6432150);
}

TEST(ParsePrice, RejectsPricesOffTheTickGrid) {
    EXPECT_FALSE(parse_price("64321.505", kCents).has_value());
}

TEST(ParsePrice, RejectsGarbage) {
    for (const char* s : {"", ".", "abc", "1.2.3", "-5", "+5", "1e5", " 1", "1 "}) {
        EXPECT_FALSE(parse_price(s, kCents).has_value()) << "input: '" << s << "'";
    }
}

TEST(ParsePrice, RejectsOverflowInsteadOfWrapping) {
    EXPECT_FALSE(parse_price("99999999999999999999", kCents).has_value());
    // fits as a whole number but not once scaled by 100
    EXPECT_FALSE(parse_price("92233720368547759", kCents).has_value());
    // the largest whole price that still fits
    EXPECT_EQ(parse_price("92233720368547758", kCents), 9223372036854775800);
}

TEST(ParsePrice, ZeroDecimalTick) {
    EXPECT_EQ(parse_price("42", TickSize{0}), 42);
    EXPECT_FALSE(parse_price("42.5", TickSize{0}).has_value());
}

TEST(FormatPrice, RoundTrips) {
    for (const char* s : {"0.00", "0.01", "64321.50", "100000.99"}) {
        auto ticks = parse_price(s, kCents);
        ASSERT_TRUE(ticks.has_value()) << s;
        EXPECT_EQ(format_price(*ticks, kCents), s);
    }
}

TEST(FormatPrice, NegativeAndExtremeValues) {
    EXPECT_EQ(format_price(-150, kCents), "-1.50");
    EXPECT_EQ(format_price(5, TickSize{0}), "5");
    // INT64_MIN has no positive counterpart; must not overflow
    EXPECT_EQ(format_price(std::numeric_limits<matchbook::Price>::min(), TickSize{0}),
              "-9223372036854775808");
}
