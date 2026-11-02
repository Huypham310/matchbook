#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "matchbook/types.hpp"

namespace matchbook {

/// Decimal places of the tick, e.g. 2 for a 0.01 tick (BTC-USD on Coinbase).
struct TickSize {
    int decimals;
};

/// Converts an exchange price string ("64321.50") to integer ticks without going through double.
/// Returns nullopt for malformed input, negative prices, overflow, or more decimals than the tick
/// allows ("64321.505" with a 0.01 tick is not a valid price, not something to round silently).
std::optional<Price> parse_price(std::string_view text, TickSize tick) noexcept;

/// Inverse of parse_price, always printing exactly `tick.decimals` decimals.
std::string format_price(Price ticks, TickSize tick);

}  // namespace matchbook
