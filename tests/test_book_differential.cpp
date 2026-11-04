// Differential test: drive the real book and a deliberately naive reference with the same random
// operations and compare everything observable after each step.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

#include "matchbook/book.hpp"

using namespace matchbook;

namespace {

// Obviously-correct and slow: a flat map of orders, everything recomputed on demand.
class ReferenceBook {
public:
    bool add(OrderId id, Side side, Price price, Qty qty) {
        if (qty <= 0 || orders_.contains(id)) return false;
        orders_[id] = {side, price, qty, seq_++};
        return true;
    }
    bool cancel(OrderId id) { return orders_.erase(id) == 1; }
    bool modify(OrderId id, Price price, Qty qty) {
        if (qty <= 0) return cancel(id);
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        auto& o = it->second;
        if (price == o.price && qty <= o.qty) {
            o.qty = qty;  // keeps its sequence number, i.e. its priority
        } else {
            o.price = price;
            o.qty = qty;
            o.seq = seq_++;
        }
        return true;
    }
    std::vector<LevelView> depth(Side side) const {
        std::map<Price, LevelView> levels;
        for (const auto& [id, o] : orders_) {
            if (o.side != side) continue;
            auto& l = levels.try_emplace(o.price, LevelView{o.price, 0, 0}).first->second;
            l.qty += o.qty;
            ++l.orders;
        }
        std::vector<LevelView> out;
        for (const auto& [p, l] : levels) out.push_back(l);
        if (side == Side::Buy) std::reverse(out.begin(), out.end());  // best first
        return out;
    }
    std::vector<OrderId> queue(Side side, Price price) const {
        std::vector<std::pair<std::uint64_t, OrderId>> q;
        for (const auto& [id, o] : orders_) {
            if (o.side == side && o.price == price) q.emplace_back(o.seq, id);
        }
        std::sort(q.begin(), q.end());
        std::vector<OrderId> ids;
        for (auto& [s, id] : q) ids.push_back(id);
        return ids;
    }
    std::size_t size() const { return orders_.size(); }

private:
    struct Ref {
        Side side;
        Price price;
        Qty qty;
        std::uint64_t seq;
    };
    std::map<OrderId, Ref> orders_;
    std::uint64_t seq_ = 0;
};

}  // namespace

class Differential : public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(Differential, MatchesReferenceOnRandomOperations) {
    std::mt19937_64 rng(GetParam());
    // narrow price band and id range: lots of shared levels, reused ids, cancels of missing ids
    std::uniform_int_distribution<Price> price(990, 1010);
    std::uniform_int_distribution<Qty> qty(-1, 20);
    std::uniform_int_distribution<OrderId> id(1, 400);
    std::uniform_int_distribution<int> op(0, 9);

    OrderBook book(1024, 256);
    ReferenceBook ref;
    for (int step = 0; step < 50'000; ++step) {
        const OrderId oid = id(rng);
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        const int which = op(rng);
        if (which < 5) {
            const Price p = price(rng);
            const Qty q = qty(rng);
            ASSERT_EQ(book.add(oid, side, p, q) == AddResult::Ok, ref.add(oid, side, p, q));
        } else if (which < 8) {
            ASSERT_EQ(book.cancel(oid), ref.cancel(oid));
        } else {
            const Price p = price(rng);
            const Qty q = qty(rng);
            ASSERT_EQ(book.modify(oid, p, q), ref.modify(oid, p, q));
        }

        ASSERT_EQ(book.order_count(), ref.size());
        if (step % 250 == 0) {
            book.check_invariants();
            for (Side s : {Side::Buy, Side::Sell}) {
                ASSERT_EQ(book.depth(s, SIZE_MAX), ref.depth(s)) << "step " << step;
            }
            // time priority: compare the queue of every level that has an order we can reach
            for (OrderId probe = 1; probe <= 400; ++probe) {
                const Order* o = book.find(probe);
                if (!o || o->level->head != o) continue;  // one probe per level is enough
                std::vector<OrderId> got;
                for (const Order* x = o; x; x = x->next) got.push_back(x->id);
                ASSERT_EQ(got, ref.queue(o->side, o->price)) << "step " << step;
            }
        }
    }
    book.check_invariants();
}

INSTANTIATE_TEST_SUITE_P(Seeds, Differential, ::testing::Values(1u, 2u, 3u, 4u));
