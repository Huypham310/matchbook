#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <string>

#include "matchbook/engine.hpp"
#include "matchbook/protocol.hpp"
#include "matchbook/replay.hpp"
#include "matchbook/session.hpp"

using namespace matchbook;
using namespace matchbook::replay;

namespace {

std::string fixture() { return (std::filesystem::path(MATCHBOOK_TEST_DATA_DIR) / "trades_small.csv").string(); }

/// Runs a stream through a session, checking after every message that the book isn't crossed.
std::size_t run_checked(const std::vector<std::byte>& stream, MatchingEngine& engine) {
    proto::Session session(engine);
    std::size_t messages = 0, crossed = 0;
    for (std::span<const std::byte> in(stream); !in.empty();) {
        const auto r = proto::decode(in);
        EXPECT_EQ(r.status, proto::DecodeStatus::Ok);
        EXPECT_EQ(session.feed(in.first(r.consumed)), proto::DecodeStatus::Ok);
        in = in.subspan(r.consumed);
        ++messages;
        const auto bid = engine.book().best_bid(), ask = engine.book().best_ask();
        if (bid && ask && *bid >= *ask) ++crossed;
    }
    EXPECT_EQ(crossed, 0u);
    return messages;
}

}  // namespace

TEST(Replay, SyntheticFlowIsDeterministicAndMatchesItsRates) {
    FlowStats s;
    const auto a = synthetic_flow({}, 200'000, 42, &s);
    const auto b = synthetic_flow({}, 200'000, 42);
    EXPECT_EQ(a, b);  // same seed, same bytes
    EXPECT_NE(a, synthetic_flow({}, 200'000, 43));
    // event types are drawn with probability rate / total = 0.6 / 0.1 / 0.3
    const auto n = static_cast<double>(s.messages);
    EXPECT_NEAR(static_cast<double>(s.limits) / n, 0.6, 0.01);
    EXPECT_NEAR(static_cast<double>(s.markets) / n, 0.1, 0.01);
    EXPECT_NEAR(static_cast<double>(s.cancels) / n, 0.3, 0.01);
}

TEST(Replay, SyntheticFlowKeepsLimitsOnTheirSideOfTheMid) {
    SyntheticConfig cfg;
    cfg.drift_prob = 0.0;  // fixed mid, so "their side" is well defined
    const auto stream = synthetic_flow(cfg, 50'000, 1);
    double qty_sum = 0;
    std::size_t limits = 0;
    for (std::span<const std::byte> in(stream); !in.empty();) {
        const auto r = proto::decode(in);
        in = in.subspan(r.consumed);
        if (r.msg.type() != proto::MsgType::NewOrder || r.msg.order_type() != OrderType::Limit) continue;
        if (r.msg.side() == Side::Buy) EXPECT_LT(r.msg.price(), cfg.mid);
        else EXPECT_GT(r.msg.price(), cfg.mid);
        qty_sum += static_cast<double>(r.msg.qty());
        ++limits;
    }
    EXPECT_NEAR(qty_sum / static_cast<double>(limits), cfg.mean_limit_qty, 0.5);
}

TEST(Replay, SyntheticFlowNeverCrossesTheBook) {
    MatchingEngine engine;
    const auto stream = synthetic_flow({}, 30'000, 9);
    EXPECT_GT(run_checked(stream, engine), 29'000u);
    EXPECT_GT(engine.trade_count(), 1'000u);
}

TEST(Replay, LoadsOneSymbolAndSkipsBadRows) {
    std::size_t bad = 0;
    const auto btc = load_trades_csv(fixture(), "BTC-USD", 0.01, 1e-8, &bad);
    EXPECT_EQ(btc.size(), 45u);
    EXPECT_EQ(bad, 3u);  // unparsable price, unknown side, short row
    EXPECT_EQ(btc[0].price, 7822658);  // 78226.58 in cents
    EXPECT_EQ(btc[0].qty, 4);          // 0.00000004 BTC in satoshi
    EXPECT_EQ(btc[0].taker, Side::Sell);
    for (std::size_t i = 1; i < btc.size(); ++i) EXPECT_LE(btc[i - 1].ts_ms, btc[i].ts_ms);

    const auto eth = load_trades_csv(fixture(), "ETH-USD", 0.01, 1e-8, &bad);
    EXPECT_EQ(eth.size(), 15u);
    EXPECT_EQ(bad, 1u);  // BTC rows are filtered out before parsing
    EXPECT_THROW(load_trades_csv("no/such/file.csv", "BTC-USD", 0.01, 1e-8), std::runtime_error);
}

TEST(Replay, TapeFlowSendsEveryPrintAsATakerAgainstFreshQuotes) {
    const auto tape = load_trades_csv(fixture(), "BTC-USD", 0.01, 1e-8);
    TapeConfig cfg;
    cfg.levels = 3;
    FlowStats s;
    const auto stream = tape_flow(tape, cfg, &s);
    EXPECT_EQ(s.markets, tape.size());
    EXPECT_EQ(s.limits, tape.size() * 6);
    EXPECT_EQ(s.cancels, (tape.size() - 1) * 6);  // the ladder is replaced before every print

    MatchingEngine engine;
    run_checked(stream, engine);
    // every taker id is in the reserved range and printed trades against a maker
    std::size_t taker_trades = 0;
    MatchingEngine e2;
    proto::Session s2(e2);
    ASSERT_EQ(s2.feed(stream), proto::DecodeStatus::Ok);
    for (std::span<const std::byte> out = s2.output(); !out.empty();) {
        const auto r = proto::decode(out);
        out = out.subspan(r.consumed);
        const Event ev = r.msg.to_event();
        if (ev.type != EventType::Trade) continue;
        EXPECT_GE(ev.id, kTakerIdBase);
        EXPECT_LT(ev.maker_id, kTakerIdBase);
        ++taker_trades;
    }
    EXPECT_GE(taker_trades, tape.size());  // each print hits at least its first level
}
