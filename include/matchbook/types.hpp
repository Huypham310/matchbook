#pragma once

#include <cstdint>
#include <string_view>

namespace matchbook {

/// Price in integer ticks. Never floating point inside the engine: comparisons must be exact,
/// and two orders at "the same" price must land on the same level.
using Price = std::int64_t;
/// Quantity in the instrument's smallest lot (e.g. satoshi-sized units for BTC).
using Qty = std::int64_t;
/// Exchange-assigned order id, unique for the lifetime of the book.
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

constexpr std::string_view to_string(Side s) noexcept { return s == Side::Buy ? "buy" : "sell"; }

/// True if an incoming order at `limit` on side `taker` can trade against a resting `level`.
/// Buyers cross when they pay at least the ask, sellers when they accept at most the bid.
constexpr bool crosses(Side taker, Price limit, Price level) noexcept {
    return taker == Side::Buy ? limit >= level : limit <= level;
}

}  // namespace matchbook
