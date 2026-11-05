#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "matchbook/book.hpp"
#include "matchbook/types.hpp"

namespace matchbook {

enum class OrderType : std::uint8_t { Limit, Market };

/// GTC rests whatever doesn't fill; IOC fills what it can now and drops the rest;
/// FOK fills completely right now or not at all.
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

struct NewOrder {
    OrderId id;
    Side side;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Price price = 0;  // ignored for market orders
    Qty qty = 0;
};

enum class RejectReason : std::uint8_t { BadQuantity, DuplicateId, BookFull, UnknownOrder };

enum class EventType : std::uint8_t {
    Trade,      // maker_id, taker_id, price, qty
    Rested,     // id, price, qty left resting on the book
    Cancelled,  // id, qty that was dropped (IOC remainder, failed FOK, user cancel)
    Rejected,   // id, reason
};

struct Event {
    EventType type;
    OrderId id = 0;        // taker / subject order
    OrderId maker_id = 0;  // trades only
    Price price = 0;
    Qty qty = 0;
    RejectReason reason = RejectReason::BadQuantity;

    bool operator==(const Event&) const = default;
};

/// Single-threaded price-time priority matcher on top of OrderBook.
///
/// Every call appends its events to `events()` (cleared at the start of the call); the vector's
/// capacity is reserved up front, so a normal call doesn't allocate.
class MatchingEngine {
public:
    explicit MatchingEngine(std::size_t max_orders = 1u << 20, std::size_t max_levels = 1u << 16);

    void submit(const NewOrder& order);
    void cancel(OrderId id);
    void modify(OrderId id, Price price, Qty qty);

    [[nodiscard]] const std::vector<Event>& events() const noexcept { return events_; }
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] std::uint64_t trade_count() const noexcept { return trades_; }

private:
    /// Fill `qty` of an incoming order against the opposite side, never past `limit`.
    /// Returns what's left unfilled.
    Qty sweep(OrderId taker, Side side, Price limit, Qty qty);
    /// Quantity available on the opposite side at prices no worse than `limit` (stops counting
    /// once `needed` is reached), for FOK.
    [[nodiscard]] Qty available(Side side, Price limit, Qty needed) const noexcept;
    void reject(OrderId id, RejectReason why);

    OrderBook book_;
    std::vector<Event> events_;
    std::uint64_t trades_ = 0;
};

}  // namespace matchbook
