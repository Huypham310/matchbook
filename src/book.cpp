#include "matchbook/book.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace matchbook {

namespace {

// Ladders are sorted from worst to best price so the best level sits at the back.
// For bids a lower price is worse, for asks a higher one.
constexpr bool worse(Side side, Price a, Price b) noexcept {
    return side == Side::Buy ? a < b : a > b;
}

auto find_slot(const std::vector<Level*>& ladder, Side side, Price price) {
    return std::lower_bound(ladder.begin(), ladder.end(), price,
                            [side](const Level* l, Price p) { return worse(side, l->price, p); });
}

[[noreturn]] void fail(const std::string& what) { throw std::logic_error("order book: " + what); }

}  // namespace

OrderBook::OrderBook(std::size_t max_orders, std::size_t max_levels)
    : orders_(max_orders), levels_(max_levels) {
    // reserve up front so growing these never allocates on the hot path
    bids_.reserve(max_levels);
    asks_.reserve(max_levels);
    index_.reserve(max_orders);
}

Level* OrderBook::level_at(Side side, Price price) const noexcept {
    const Ladder& l = ladder(side);
    auto it = find_slot(l, side, price);
    return (it != l.end() && (*it)->price == price) ? *it : nullptr;
}

Level* OrderBook::get_or_create_level(Side side, Price price) {
    Ladder& l = ladder(side);
    auto it = find_slot(l, side, price);
    if (it != l.end() && (*it)->price == price) return *it;
    Level* lvl = levels_.acquire();
    if (!lvl) return nullptr;
    lvl->price = price;
    // a new best price lands at the back: plain push_back, no shifting
    l.insert(it, lvl);
    return lvl;
}

AddResult OrderBook::add(OrderId id, Side side, Price price, Qty qty) {
    if (qty <= 0) return AddResult::BadQuantity;
    if (index_.contains(id)) return AddResult::DuplicateId;
    Order* o = orders_.acquire();
    if (!o) return AddResult::BookFull;
    Level* lvl = get_or_create_level(side, price);
    if (!lvl) {
        orders_.release(o);
        return AddResult::BookFull;
    }
    o->id = id;
    o->side = side;
    o->price = price;
    o->qty = qty;
    lvl->push_back(o);
    index_.emplace(id, o);
    return AddResult::Ok;
}

void OrderBook::unlink(Order* o) {
    Level* lvl = o->level;
    const Side side = o->side;
    lvl->erase(o);
    if (lvl->empty()) {
        Ladder& l = ladder(side);
        // usually the top level, i.e. the last element
        if (l.back() == lvl) {
            l.pop_back();
        } else {
            l.erase(find_slot(l, side, lvl->price));
        }
        levels_.release(lvl);
    }
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Order* o = it->second;
    unlink(o);
    index_.erase(it);
    orders_.release(o);
    return true;
}

bool OrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
    if (new_qty <= 0) return cancel(id);
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    Order* o = it->second;

    if (new_price == o->price && new_qty <= o->qty) {
        // shrinking in place keeps the order's spot in the queue
        o->level->total_qty -= o->qty - new_qty;
        o->qty = new_qty;
        return true;
    }

    // price change or size increase: back of the queue at the new price
    unlink(o);
    o->price = new_price;
    o->qty = new_qty;
    Level* lvl = get_or_create_level(o->side, new_price);
    if (!lvl) {
        // no level slot left: the order can't rest anywhere, drop it instead of corrupting state
        index_.erase(it);
        orders_.release(o);
        return false;
    }
    lvl->push_back(o);
    return true;
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.back()->price;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.back()->price;
}

Qty OrderBook::volume_at(Side side, Price price) const noexcept {
    const Level* lvl = level_at(side, price);
    return lvl ? lvl->total_qty : 0;
}

std::vector<LevelView> OrderBook::depth(Side side, std::size_t n) const {
    const Ladder& l = ladder(side);
    std::vector<LevelView> out;
    out.reserve(std::min(n, l.size()));
    for (auto it = l.rbegin(); it != l.rend() && out.size() < n; ++it) {
        out.push_back({(*it)->price, (*it)->total_qty, (*it)->count});
    }
    return out;
}

const Order* OrderBook::find(OrderId id) const noexcept {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : it->second;
}

std::size_t OrderBook::level_count(Side side) const noexcept { return ladder(side).size(); }

void OrderBook::check_invariants() const {
    std::size_t orders_seen = 0;
    for (Side side : {Side::Buy, Side::Sell}) {
        const Ladder& l = ladder(side);
        for (std::size_t i = 0; i < l.size(); ++i) {
            const Level* lvl = l[i];
            if (i > 0 && !worse(side, l[i - 1]->price, lvl->price)) fail("ladder not sorted");
            if (lvl->empty() || lvl->count == 0) fail("empty level left in the ladder");

            Qty qty = 0;
            std::uint32_t count = 0;
            const Order* prev = nullptr;
            for (const Order* o = lvl->head; o; o = o->next) {
                if (o->prev != prev) fail("broken prev link");
                if (o->level != lvl || o->price != lvl->price || o->side != side) {
                    fail("order " + std::to_string(o->id) + " filed under the wrong level");
                }
                if (o->qty <= 0) fail("non-positive quantity resting");
                auto it = index_.find(o->id);
                if (it == index_.end() || it->second != o) fail("order missing from the index");
                qty += o->qty;
                ++count;
                prev = o;
            }
            if (prev != lvl->tail) fail("tail doesn't match the last order");
            if (qty != lvl->total_qty || count != lvl->count) fail("level totals out of sync");
            orders_seen += count;
        }
    }
    if (orders_seen != index_.size()) fail("index has orders that aren't in any level");
    if (orders_.in_use() != index_.size()) fail("order pool leak");
    if (levels_.in_use() != bids_.size() + asks_.size()) fail("level pool leak");
}

}  // namespace matchbook
