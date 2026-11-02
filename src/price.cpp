#include "matchbook/price.hpp"

#include <cstdint>
#include <limits>

namespace matchbook {

namespace {

// 10^n for small n, enough for any realistic tick size
constexpr std::int64_t pow10(int n) {
    std::int64_t r = 1;
    for (int i = 0; i < n; ++i) r *= 10;
    return r;
}

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

}  // namespace

std::optional<Price> parse_price(std::string_view text, TickSize tick) noexcept {
    if (text.empty() || tick.decimals < 0 || tick.decimals > 12) return std::nullopt;

    std::int64_t whole = 0;
    std::size_t i = 0;
    bool any_digit = false;
    for (; i < text.size() && text[i] != '.'; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return std::nullopt;
        // tràn số thì trả lỗi, không để wrap âm
        if (whole > (kMax - (c - '0')) / 10) return std::nullopt;
        whole = whole * 10 + (c - '0');
        any_digit = true;
    }

    std::int64_t frac = 0;
    int frac_digits = 0;
    if (i < text.size()) {  // skip '.'
        ++i;
        for (; i < text.size(); ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') return std::nullopt;
            any_digit = true;
            if (frac_digits < tick.decimals) {
                frac = frac * 10 + (c - '0');
                ++frac_digits;
            } else if (c != '0') {
                // finer than the tick: "64321.505" isn't on the 0.01 grid
                return std::nullopt;
            }
        }
    }
    if (!any_digit) return std::nullopt;

    // "64321.5" with 2 decimals means 50 hundredths, not 5
    frac *= pow10(tick.decimals - frac_digits);
    const std::int64_t scale = pow10(tick.decimals);
    if (whole > (kMax - frac) / scale) return std::nullopt;
    return whole * scale + frac;
}

std::string format_price(Price ticks, TickSize tick) {
    const std::int64_t scale = pow10(tick.decimals);
    std::string out = ticks < 0 ? "-" : "";
    // careful with INT64_MIN: work on the unsigned magnitude
    const std::uint64_t mag = ticks < 0 ? 0 - static_cast<std::uint64_t>(ticks)
                                        : static_cast<std::uint64_t>(ticks);
    out += std::to_string(mag / static_cast<std::uint64_t>(scale));
    if (tick.decimals > 0) {
        std::string frac = std::to_string(mag % static_cast<std::uint64_t>(scale));
        out += '.';
        out.append(static_cast<std::size_t>(tick.decimals) - frac.size(), '0');
        out += frac;
    }
    return out;
}

}  // namespace matchbook
