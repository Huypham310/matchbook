#include "matchbook/replay.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include "matchbook/protocol.hpp"

namespace matchbook::replay {

namespace {

class Encoder {
public:
    explicit Encoder(std::size_t reserve) { buf_.reserve(reserve * 40); }

    void add(const NewOrder& o) { put(40, [&](std::span<std::byte> d) {
        return proto::encode_new_order(d, o, seq_++); }); }  // fmt: skip
    void cancel(OrderId id) { put(16, [&](std::span<std::byte> d) {
        return proto::encode_cancel(d, id, seq_++); }); }  // fmt: skip

    std::vector<std::byte> take() { return std::move(buf_); }

private:
    template <class F>
    void put(std::size_t n, F&& encode) {
        const std::size_t at = buf_.size();
        buf_.resize(at + n);
        buf_.resize(at + encode(std::span<std::byte>(buf_.data() + at, n)));
    }

    std::vector<std::byte> buf_;
    std::uint32_t seq_ = 1;
};

}  // namespace

std::vector<std::byte> synthetic_flow(const SyntheticConfig& cfg, std::size_t events,
                                      std::uint64_t seed, FlowStats* stats) {
    std::mt19937_64 rng(seed);
    const double total = cfg.limit_rate + cfg.market_rate + cfg.cancel_rate;
    std::uniform_real_distribution<double> u(0.0, total);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    // geometric with mean m on {1, 2, ...}: success probability 1/m
    std::geometric_distribution<long> level(cfg.level_decay);
    std::geometric_distribution<long> lqty(1.0 / cfg.mean_limit_qty);
    std::geometric_distribution<long> mqty(1.0 / cfg.mean_market_qty);

    Encoder out(events);
    FlowStats s;
    std::vector<OrderId> alive;  // resting candidates for cancels; filled ones just get rejected
    OrderId next = 1;
    Price mid = cfg.mid;
    for (std::size_t i = 0; i < events; ++i) {
        const double x = u(rng);
        const Side side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
        if (x < cfg.limit_rate) {
            const Price d = 1 + level(rng);
            const Price px = side == Side::Buy ? mid - d : mid + d;
            out.add({next, side, OrderType::Limit, TimeInForce::GTC, px, 1 + lqty(rng)});
            alive.push_back(next++);
            ++s.limits;
        } else if (x < cfg.limit_rate + cfg.market_rate) {
            out.add({next++, side, OrderType::Market, TimeInForce::IOC, 0, 1 + mqty(rng)});
            ++s.markets;
        } else if (!alive.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, alive.size() - 1);
            const std::size_t k = pick(rng);
            out.cancel(alive[k]);
            alive[k] = alive.back();  // swap-remove: O(1), order doesn't matter
            alive.pop_back();
            ++s.cancels;
        } else {
            continue;  // nothing to cancel yet
        }
        if (unit(rng) < cfg.drift_prob) mid += unit(rng) < 0.5 ? -1 : 1;
    }
    s.messages = s.limits + s.markets + s.cancels;
    if (stats) *stats = s;
    return out.take();
}

std::vector<TapeTrade> load_trades_csv(const std::string& path, std::string_view symbol,
                                       double tick_size, double lot_size, std::size_t* bad_rows) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<TapeTrade> out;
    std::size_t bad = 0;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::stringstream ss(line);
        std::string ts, sym, price, size, side;
        if (!std::getline(ss, ts, ',') || !std::getline(ss, sym, ',') ||
            !std::getline(ss, price, ',') || !std::getline(ss, size, ',') ||
            !std::getline(ss, side, ',')) {
            ++bad;
            continue;
        }
        if (sym != symbol) continue;
        try {
            const double p = std::stod(price), q = std::stod(size);
            const auto qty = static_cast<Qty>(std::llround(q / lot_size));
            if (!(p > 0) || qty <= 0 || (side != "buy" && side != "sell")) {
                ++bad;
                continue;
            }
            out.push_back({std::stoll(ts), static_cast<Price>(std::llround(p / tick_size)), qty,
                           side == "buy" ? Side::Buy : Side::Sell});
        } catch (const std::exception&) {
            ++bad;  // stod / stoll on garbage
        }
    }
    if (bad_rows) *bad_rows = bad;
    return out;
}

std::vector<std::byte> tape_flow(const std::vector<TapeTrade>& tape, const TapeConfig& cfg,
                                 FlowStats* stats) {
    Qty quote = cfg.quote_qty;
    if (quote <= 0 && !tape.empty()) {
        std::vector<Qty> q;
        q.reserve(tape.size());
        for (const auto& t : tape) q.push_back(t.qty);
        std::nth_element(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(q.size() / 2), q.end());
        quote = std::max<Qty>(q[q.size() / 2], 1);
    }
    Encoder out(tape.size() * static_cast<std::size_t>(4 * cfg.levels + 1));
    FlowStats s;
    std::vector<OrderId> quotes;
    OrderId maker = 1;
    for (std::size_t i = 0; i < tape.size(); ++i) {
        const TapeTrade& t = tape[i];
        // makers re-centre on the last print: pull the old ladder, post a fresh one
        for (OrderId id : quotes) {
            out.cancel(id);
            ++s.cancels;
        }
        quotes.clear();
        const Price ref = i > 0 ? tape[i - 1].price : t.price;
        for (int k = 1; k <= cfg.levels; ++k) {
            out.add({maker, Side::Buy, OrderType::Limit, TimeInForce::GTC, ref - k, quote});
            quotes.push_back(maker++);
            out.add({maker, Side::Sell, OrderType::Limit, TimeInForce::GTC, ref + k, quote});
            quotes.push_back(maker++);
            s.limits += 2;
        }
        out.add({kTakerIdBase + i, t.taker, OrderType::Market, TimeInForce::IOC, 0, t.qty});
        ++s.markets;
    }
    s.messages = s.limits + s.markets + s.cancels;
    if (stats) *stats = s;
    return out.take();
}

}  // namespace matchbook::replay
