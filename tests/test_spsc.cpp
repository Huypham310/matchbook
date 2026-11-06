#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "matchbook/pipeline.hpp"
#include "matchbook/protocol.hpp"
#include "matchbook/session.hpp"
#include "matchbook/spsc.hpp"

using namespace matchbook;

TEST(Spsc, FullEmptyAndWraparound) {
    SpscRing<int, 4> q;
    int v = 0;
    EXPECT_FALSE(q.try_pop(v));
    for (int round = 0; round < 5; ++round) {  // counters keep growing past N several times
        for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(round * 10 + i));
        EXPECT_FALSE(q.try_push(99));  // all 4 slots usable, the 5th push fails
        EXPECT_EQ(q.size_approx(), 4u);
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(q.try_pop(v));
            EXPECT_EQ(v, round * 10 + i);
        }
        EXPECT_FALSE(q.try_pop(v));
    }
}

TEST(Spsc, PaddedLayoutPutsCountersOnSeparateLines) {
    // the whole point of padding: head and tail mustn't share a cache line
    EXPECT_GE(sizeof(SpscRing<char, 2, true>), 4 * kCacheLine);
    EXPECT_LT(sizeof(SpscRing<char, 2, false>), kCacheLine);
}

template <bool Padded>
void two_thread_stress() {
    constexpr std::uint64_t kCount = 2'000'000;
    auto q = std::make_unique<SpscRing<std::uint64_t, 1024, Padded>>();
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount;) {
            if (q->try_push(i)) ++i;
        }
    });
    std::uint64_t expected = 0, sum = 0, v = 0;
    bool in_order = true;
    while (expected < kCount) {
        if (!q->try_pop(v)) continue;
        in_order = in_order && v == expected;
        sum += v;
        ++expected;
    }
    producer.join();
    EXPECT_TRUE(in_order);  // nothing lost, duplicated or reordered
    EXPECT_EQ(sum, kCount * (kCount - 1) / 2);
}

TEST(Spsc, TwoThreadsPaddedRing) { two_thread_stress<true>(); }
TEST(Spsc, TwoThreadsPlainRing) { two_thread_stress<false>(); }

TEST(Spsc, StructPayloadsArriveWhole) {
    struct Msg {
        std::uint64_t a, b, c;  // a torn read would break a + b == c
    };
    auto q = std::make_unique<SpscRing<Msg, 256>>();
    constexpr std::uint64_t kCount = 500'000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount;) {
            if (q->try_push({i, 2 * i, 3 * i})) ++i;
        }
    });
    Msg m{};
    std::uint64_t got = 0, bad = 0;
    while (got < kCount) {
        if (!q->try_pop(m)) continue;
        bad += (m.a + m.b != m.c) || m.a != got;
        ++got;
    }
    producer.join();
    EXPECT_EQ(bad, 0u);
}

namespace {

std::vector<std::byte> random_stream(int n, std::uint32_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::byte> out(static_cast<std::size_t>(n) * 40);
    std::size_t at = 0;
    for (int i = 0; i < n; ++i) {
        std::span<std::byte> dst(out.data() + at, out.size() - at);
        const auto seq = static_cast<std::uint32_t>(i);
        const auto r = rng() % 20;
        if (i > 0 && r == 0) {
            at += proto::encode_cancel(dst, 1 + rng() % static_cast<std::uint64_t>(i), seq);
        } else if (i > 0 && r == 1) {
            at += proto::encode_modify(dst, 1 + rng() % static_cast<std::uint64_t>(i),
                                       static_cast<Price>(95 + rng() % 11),
                                       static_cast<Qty>(1 + rng() % 30), seq);
        } else {
            const NewOrder o{static_cast<OrderId>(i + 1), rng() % 2 ? Side::Buy : Side::Sell,
                             rng() % 30 == 0 ? OrderType::Market : OrderType::Limit,
                             rng() % 6 == 0 ? TimeInForce::IOC : TimeInForce::GTC,
                             static_cast<Price>(95 + rng() % 11), static_cast<Qty>(1 + rng() % 30)};
            at += proto::encode_new_order(dst, o, seq);
        }
    }
    out.resize(at);
    return out;
}

}  // namespace

TEST(Pipeline, ThreeThreadsGiveTheSameBytesAsOneThread) {
    const auto stream = random_stream(50'000, 21);

    MatchingEngine engine;
    proto::Session session(engine);
    ASSERT_EQ(session.feed(stream), proto::DecodeStatus::Ok);
    const auto expected = session.output();

    Pipeline pipeline;
    const auto r = pipeline.run(stream);
    EXPECT_EQ(r.messages, 50'000u);
    EXPECT_EQ(r.trades, engine.trade_count());
    EXPECT_GT(r.trades, 10'000u);
    ASSERT_EQ(r.output.size(), expected.size());
    EXPECT_TRUE(std::equal(r.output.begin(), r.output.end(), expected.begin()));
    // one latency sample per message: every message produces at least one event
    EXPECT_EQ(r.latency_ns.size(), r.messages);
}

TEST(Pipeline, PinnedAndPacedRunsGiveTheSameOutput) {
    if (std::thread::hardware_concurrency() < 3) GTEST_SKIP() << "needs 3 CPUs to pin to";
    const auto stream = random_stream(5'000, 8);
    const auto free_run = Pipeline{}.run(stream);
    Pipeline pinned;
    pinned.pin_threads({0, 1, 2});
    const auto r = pinned.run(stream, true, 2e6);
    EXPECT_EQ(r.output, free_run.output);
    // pacing at 2M msg/s: 5000 messages can't finish faster than 2.5 ms
    EXPECT_GE(r.seconds, 0.0025);
}

TEST(Pipeline, StopsAtTheFirstBadMessageAndStillJoins) {
    auto stream = random_stream(100, 4);
    std::size_t at = 0;
    for (int i = 0; i < 10; ++i) at += proto::decode(std::span(stream).subspan(at)).consumed;
    stream[at + 2] = std::byte{'?'};
    const auto r = Pipeline{}.run(stream);
    EXPECT_EQ(r.messages, 10u);
}

TEST(Pipeline, Percentiles) {
    std::vector<std::uint32_t> v(100);
    for (std::uint32_t i = 0; i < 100; ++i) v[i] = 100 - i;  // 100 .. 1, unsorted
    EXPECT_EQ(percentile(v, 50), 50u);
    EXPECT_EQ(percentile(v, 99), 99u);
    EXPECT_EQ(percentile(v, 100), 100u);
    EXPECT_EQ(percentile({}, 50), 0u);
}
