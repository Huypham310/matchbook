// The engine against a deliberately slow reference matcher: same random order flow, the exact
// same event stream, the same book after every step, and the book never crossed.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <vector>

#include "matchbook/engine.hpp"

using namespace matchbook;

namespace {

class ReferenceMatcher {
public:
    std::vector<Event> submit(const NewOrder& o) {
        std::vector<Event> ev;
        if (o.qty <= 0) return {{EventType::Rejected, o.id, 0, 0, 0, RejectReason::BadQuantity}};
        if (resting_.contains(o.id)) {
            return {{EventType::Rejected, o.id, 0, 0, 0, RejectReason::DuplicateId}};
        }
        const bool market = o.type == OrderType::Market;
        const Price limit = market ? (o.side == Side::Buy ? std::numeric_limits<Price>::max()
                                                          : std::numeric_limits<Price>::min())
                                   : o.price;
        auto makers = crossing(o.side, limit);
        Qty avail = 0;
        for (auto id : makers) avail += resting_[id].qty;
        if (o.tif == TimeInForce::FOK && avail < o.qty) {
            return {{EventType::Cancelled, o.id, 0, o.price, o.qty, {}}};
        }
        Qty left = o.qty;
        for (auto id : makers) {
            if (left == 0) break;
            auto& m = resting_[id];
            const Qty f = std::min(left, m.qty);
            ev.push_back({EventType::Trade, o.id, id, m.price, f, {}});
            left -= f;
            m.qty -= f;
            if (m.qty == 0) resting_.erase(id);
        }
        if (left > 0) {
            if (!market && o.tif == TimeInForce::GTC) {
                resting_[o.id] = {o.side, o.price, left, seq_++};
                ev.push_back({EventType::Rested, o.id, 0, o.price, left, {}});
            } else {
                ev.push_back({EventType::Cancelled, o.id, 0, o.price, left, {}});
            }
        }
        return ev;
    }

    std::vector<Event> cancel(OrderId id) {
        auto it = resting_.find(id);
        if (it == resting_.end()) {
            return {{EventType::Rejected, id, 0, 0, 0, RejectReason::UnknownOrder}};
        }
        Event e{EventType::Cancelled, id, 0, it->second.price, it->second.qty, {}};
        resting_.erase(it);
        return {e};
    }

    std::vector<Event> modify(OrderId id, Price price, Qty qty) {
        auto it = resting_.find(id);
        if (it == resting_.end()) {
            return {{EventType::Rejected, id, 0, 0, 0, RejectReason::UnknownOrder}};
        }
        if (qty <= 0) return cancel(id);
        auto& o = it->second;
        auto opp = crossing(o.side, price);
        if (price != o.price && !opp.empty()) {
            const Side s = o.side;
            resting_.erase(it);
            return submit({id, s, OrderType::Limit, TimeInForce::GTC, price, qty});
        }
        if (!(price == o.price && qty <= o.qty)) o.seq = seq_++;  // loses priority
        o.price = price;
        o.qty = qty;
        return {{EventType::Rested, id, 0, price, qty, {}}};
    }

    std::vector<LevelView> depth(Side side) const {
        std::map<Price, LevelView> levels;
        for (const auto& [id, o] : resting_) {
            if (o.side != side) continue;
            auto& l = levels.try_emplace(o.price, LevelView{o.price, 0, 0}).first->second;
            l.qty += o.qty;
            ++l.orders;
        }
        std::vector<LevelView> out;
        for (const auto& [p, l] : levels) out.push_back(l);
        if (side == Side::Buy) std::reverse(out.begin(), out.end());
        return out;
    }

private:
    struct Ref {
        Side side;
        Price price;
        Qty qty;
        std::uint64_t seq;
    };

    // resting orders on the other side that `side` at `limit` can trade with, in match order
    std::vector<OrderId> crossing(Side side, Price limit) const {
        std::vector<std::pair<std::pair<Price, std::uint64_t>, OrderId>> v;
        for (const auto& [id, o] : resting_) {
            if (o.side == side || !crosses(side, limit, o.price)) continue;
            // best price first (low asks for a buyer, high bids for a seller), then oldest
            const Price key = side == Side::Buy ? o.price : -o.price;
            v.push_back({{key, o.seq}, id});
        }
        std::sort(v.begin(), v.end());
        std::vector<OrderId> ids;
        for (auto& [k, id] : v) ids.push_back(id);
        return ids;
    }

    std::map<OrderId, Ref> resting_;
    std::uint64_t seq_ = 0;
};

}  // namespace

class EngineDifferential : public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(EngineDifferential, SameEventsSameBook) {
    std::mt19937_64 rng(GetParam());
    std::uniform_int_distribution<Price> price(95, 105);
    std::uniform_int_distribution<Qty> qty(0, 15);
    std::uniform_int_distribution<OrderId> id(1, 300);
    std::uniform_int_distribution<int> pick(0, 99);

    MatchingEngine engine(1024, 256);
    ReferenceMatcher ref;
    std::uint64_t trades = 0;
    for (int step = 0; step < 30'000; ++step) {
        const int r = pick(rng);
        const OrderId oid = id(rng);
        std::vector<Event> expected;
        if (r < 70) {
            NewOrder o{oid, (rng() & 1) ? Side::Buy : Side::Sell};
            o.type = r < 8 ? OrderType::Market : OrderType::Limit;
            o.tif = r < 50 ? TimeInForce::GTC : (r < 60 ? TimeInForce::IOC : TimeInForce::FOK);
            o.price = o.type == OrderType::Market ? 0 : price(rng);
            o.qty = qty(rng);
            engine.submit(o);
            expected = ref.submit(o);
        } else if (r < 88) {
            engine.cancel(oid);
            expected = ref.cancel(oid);
        } else {
            const Price p = price(rng);
            const Qty q = qty(rng);
            engine.modify(oid, p, q);
            expected = ref.modify(oid, p, q);
        }
        ASSERT_EQ(engine.events(), expected) << "step " << step;
        for (const auto& e : expected) trades += e.type == EventType::Trade;

        auto bid = engine.book().best_bid();
        auto ask = engine.book().best_ask();
        ASSERT_TRUE(!bid || !ask || *bid < *ask) << "crossed book at step " << step;
        if (step % 500 == 0) {
            engine.book().check_invariants();
            ASSERT_EQ(engine.book().depth(Side::Buy, SIZE_MAX), ref.depth(Side::Buy));
            ASSERT_EQ(engine.book().depth(Side::Sell, SIZE_MAX), ref.depth(Side::Sell));
        }
    }
    EXPECT_EQ(engine.trade_count(), trades);
    EXPECT_GT(trades, 10'000u);  // the flow really exercises matching, not just resting
}

INSTANTIATE_TEST_SUITE_P(Seeds, EngineDifferential, ::testing::Values(11u, 12u, 13u, 14u));
