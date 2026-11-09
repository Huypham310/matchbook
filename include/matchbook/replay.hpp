#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "matchbook/types.hpp"

namespace matchbook::replay {

/// Synthetic order flow: limit orders arrive around a drifting mid, marketable orders and
/// cancels arrive at their own rates. Event types are drawn with probability proportional to
/// their rate (a merged Poisson process), so the mix and the per-type counts match the rates.
struct SyntheticConfig {
    double limit_rate = 0.60;   // relative rates; only the ratios matter
    double market_rate = 0.10;
    double cancel_rate = 0.30;
    Price mid = 1'000'000;       // in ticks
    double level_decay = 0.35;   // limit distance from mid: 1 + Geometric(level_decay) ticks
    double mean_limit_qty = 10;  // Geometric quantities
    double mean_market_qty = 25;
    double drift_prob = 0.01;    // chance per event that the mid moves one tick
};

struct FlowStats {
    std::size_t messages = 0, limits = 0, markets = 0, cancels = 0;
};

/// Encoded wire messages (see protocol.hpp) for `events` events.
std::vector<std::byte> synthetic_flow(const SyntheticConfig& cfg, std::size_t events,
                                      std::uint64_t seed, FlowStats* stats = nullptr);

/// One print from an exchange's public trade feed, scaled to integer ticks and lots.
struct TapeTrade {
    std::int64_t ts_ms;
    Price price;
    Qty qty;
    Side taker;
};

/// Reads `ts_ms,symbol,price,size,side` rows (tools/export_trades.py) for one symbol.
/// Malformed rows are skipped and counted in `bad_rows`.
std::vector<TapeTrade> load_trades_csv(const std::string& path, std::string_view symbol,
                                       double tick_size, double lot_size,
                                       std::size_t* bad_rows = nullptr);

/// Replays real prints as taker orders against synthetic market makers.
///
/// Before each print the makers quote `levels` price levels per side around the last print
/// (one tick apart, `quote_qty` each, refreshed every print, old quotes cancelled), then the
/// print itself is sent as an IOC order of the real size and side. Taker order ids start at
/// `kTakerIdBase`, so their fills can be told apart in the output.
struct TapeConfig {
    int levels = 5;
    Qty quote_qty = 0;  // 0 = the median print size
};

inline constexpr OrderId kTakerIdBase = OrderId{1} << 40;

std::vector<std::byte> tape_flow(const std::vector<TapeTrade>& tape, const TapeConfig& cfg,
                                 FlowStats* stats = nullptr);

}  // namespace matchbook::replay
