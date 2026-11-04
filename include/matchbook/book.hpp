#pragma once

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "matchbook/order.hpp"
#include "matchbook/pool.hpp"
#include "matchbook/types.hpp"

namespace matchbook {

enum class AddResult { Ok, DuplicateId, BadQuantity, BookFull };

struct LevelView {
    Price price;
    Qty qty;
    std::uint32_t orders;

    bool operator==(const LevelView&) const = default;
};

/// Limit order book for one instrument: resting orders only (matching lives on top of it).
///
/// Layout: each side keeps its levels in a sorted vector with the BEST price at the back, so the
/// common cases (new order at or near the top, top level emptying) are push_back / pop_back.
/// Orders and levels come from fixed pools; cancel and modify find the order through an id index
/// and unlink it from its level in O(1).
class OrderBook {
public:
    explicit OrderBook(std::size_t max_orders = 1u << 20, std::size_t max_levels = 1u << 16);

    AddResult add(OrderId id, Side side, Price price, Qty qty);
    /// False if the id isn't resting in the book.
    bool cancel(OrderId id);
    /// Same price and a smaller quantity keeps time priority (exchanges allow that); anything else
    /// (price change or size increase) re-queues the order at the back of its new level.
    bool modify(OrderId id, Price new_price, Qty new_qty);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] Qty volume_at(Side side, Price price) const noexcept;
    /// Top `n` levels of one side, best first.
    [[nodiscard]] std::vector<LevelView> depth(Side side, std::size_t n) const;
    [[nodiscard]] const Order* find(OrderId id) const noexcept;
    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t level_count(Side side) const noexcept;

    /// Walks every structure and throws std::logic_error on the first inconsistency. Slow, meant
    /// for tests and debugging.
    void check_invariants() const;

private:
    // the matching engine works directly on levels and orders to avoid lookups on the hot path
    friend class MatchingEngine;

    using Ladder = std::vector<Level*>;

    [[nodiscard]] Ladder& ladder(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
    [[nodiscard]] const Ladder& ladder(Side s) const noexcept {
        return s == Side::Buy ? bids_ : asks_;
    }
    [[nodiscard]] Level* level_at(Side side, Price price) const noexcept;
    Level* get_or_create_level(Side side, Price price);
    void unlink(Order* o);

    Pool<Order> orders_;
    Pool<Level> levels_;
    Ladder bids_;  // ascending price, best (highest) at the back
    Ladder asks_;  // descending price, best (lowest) at the back
    std::unordered_map<OrderId, Order*> index_;
};

}  // namespace matchbook
