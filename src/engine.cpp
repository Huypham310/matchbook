#include "matchbook/engine.hpp"

#include <algorithm>
#include <limits>

namespace matchbook {

namespace {

// a market order is a limit order at the most aggressive price possible
constexpr Price market_limit(Side side) noexcept {
    return side == Side::Buy ? std::numeric_limits<Price>::max()
                             : std::numeric_limits<Price>::min();
}

}  // namespace

MatchingEngine::MatchingEngine(std::size_t max_orders, std::size_t max_levels)
    : book_(max_orders, max_levels) {
    // enough for any ordinary order; a sweep through thousands of resting orders may still grow it
    events_.reserve(4096);
}

void MatchingEngine::reject(OrderId id, RejectReason why) {
    events_.push_back({EventType::Rejected, id, 0, 0, 0, why});
}

Qty MatchingEngine::available(Side side, Price limit, Qty needed) const noexcept {
    const auto& ladder = book_.ladder(opposite(side));
    Qty total = 0;
    for (auto it = ladder.rbegin(); it != ladder.rend() && total < needed; ++it) {
        if (!crosses(side, limit, (*it)->price)) break;
        total += (*it)->total_qty;
    }
    return total;
}

Qty MatchingEngine::sweep(OrderId taker, Side side, Price limit, Qty qty) {
    auto& ladder = book_.ladder(opposite(side));
    while (qty > 0 && !ladder.empty()) {
        Level* lvl = ladder.back();  // best opposite price
        if (!crosses(side, limit, lvl->price)) break;
        const Price px = lvl->price;  // trades happen at the resting (maker) price
        while (qty > 0) {
            Order* maker = lvl->head;  // oldest order at the level: time priority
            const Qty fill = std::min(qty, maker->qty);
            events_.push_back({EventType::Trade, taker, maker->id, px, fill, {}});
            ++trades_;
            qty -= fill;
            maker->qty -= fill;
            lvl->total_qty -= fill;
            if (maker->qty > 0) break;  // taker is done, maker keeps the rest
            // maker fully filled: if it was the last one, unlink() frees the level too
            const bool last_in_level = lvl->count == 1;
            book_.unlink(maker);
            book_.index_.erase(maker->id);
            book_.orders_.release(maker);
            if (last_in_level) break;
        }
    }
    return qty;
}

void MatchingEngine::submit(const NewOrder& o) {
    events_.clear();
    if (o.qty <= 0) return reject(o.id, RejectReason::BadQuantity);
    if (book_.index_.contains(o.id)) return reject(o.id, RejectReason::DuplicateId);

    const bool market = o.type == OrderType::Market;
    const Price limit = market ? market_limit(o.side) : o.price;

    if (o.tif == TimeInForce::FOK && available(o.side, limit, o.qty) < o.qty) {
        // all or nothing: checked before touching the book, so a failed FOK leaves no trace
        events_.push_back({EventType::Cancelled, o.id, 0, o.price, o.qty, {}});
        return;
    }

    const Qty left = sweep(o.id, o.side, limit, o.qty);
    if (left == 0) return;

    if (!market && o.tif == TimeInForce::GTC) {
        if (book_.add(o.id, o.side, o.price, left) == AddResult::Ok) {
            events_.push_back({EventType::Rested, o.id, 0, o.price, left, {}});
        } else {
            reject(o.id, RejectReason::BookFull);  // trades above still stand
        }
    } else {
        // IOC / market: whatever didn't fill now is gone
        events_.push_back({EventType::Cancelled, o.id, 0, o.price, left, {}});
    }
}

void MatchingEngine::cancel(OrderId id) {
    events_.clear();
    const Order* o = book_.find(id);
    if (!o) return reject(id, RejectReason::UnknownOrder);
    const Qty qty = o->qty;
    const Price px = o->price;
    book_.cancel(id);
    events_.push_back({EventType::Cancelled, id, 0, px, qty, {}});
}

void MatchingEngine::modify(OrderId id, Price price, Qty qty) {
    events_.clear();
    const Order* o = book_.find(id);
    if (!o) return reject(id, RejectReason::UnknownOrder);
    if (qty <= 0) return cancel(id);

    const Side side = o->side;
    const auto& opp = book_.ladder(opposite(side));
    if (price != o->price && !opp.empty() && crosses(side, price, opp.back()->price)) {
        // repriced through the spread: it's an aggressive order now, match it like a new one
        book_.cancel(id);
        submit({id, side, OrderType::Limit, TimeInForce::GTC, price, qty});
        return;
    }
    if (!book_.modify(id, price, qty)) return reject(id, RejectReason::BookFull);
    events_.push_back({EventType::Rested, id, 0, price, qty, {}});
}

}  // namespace matchbook
