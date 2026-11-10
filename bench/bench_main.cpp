// Google Benchmark suite: the book and engine operations one at a time, the wire protocol, a
// whole synthetic session, and the SPSC ring.
//
//   ./build/release/bench/matchbook_bench --benchmark_min_time=0.5s
//
// Each benchmark sets up its state outside the timed loop and pauses the timer for resets, so
// the reported time is the operation alone.

#include <benchmark/benchmark.h>

#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "matchbook/book.hpp"
#include "matchbook/engine.hpp"
#include "matchbook/protocol.hpp"
#include "matchbook/replay.hpp"
#include "matchbook/session.hpp"
#include "matchbook/spsc.hpp"

using namespace matchbook;

namespace {

constexpr Price kMid = 100'000;

/// A book with `levels` levels per side, `per_level` orders each, ids from 1.
void fill(OrderBook& book, int levels, int per_level, OrderId& next) {
    for (int l = 1; l <= levels; ++l) {
        for (int k = 0; k < per_level; ++k) {
            book.add(next++, Side::Buy, kMid - l, 10);
            book.add(next++, Side::Sell, kMid + l, 10);
        }
    }
}

}  // namespace

// add + cancel of one order near the top of a busy book: the bread-and-butter path of a market
// maker re-quoting
static void BM_AddCancel(benchmark::State& state) {
    OrderBook book(1u << 16, 1u << 12);
    OrderId next = 1;
    fill(book, 20, 5, next);
    std::mt19937 rng(1);
    for (auto _ : state) {
        const OrderId id = next++;
        const Price px = kMid - 1 - static_cast<Price>(rng() % 20);
        benchmark::DoNotOptimize(book.add(id, Side::Buy, px, 10));
        benchmark::DoNotOptimize(book.cancel(id));
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_AddCancel);

// a passive limit order that rests without trading, then its cancel, through the engine (events
// included). Timed as a pair: PauseTiming() costs ~200 ns on this machine, more than the work.
static void BM_EnginePassiveLimitCancel(benchmark::State& state) {
    MatchingEngine engine(1u << 16, 1u << 12);
    OrderId next = 1;
    for (auto _ : state) {
        const OrderId id = next++;
        engine.submit({id, Side::Buy, OrderType::Limit, TimeInForce::GTC,
                       kMid - 1 - static_cast<Price>(id % 50), 10});
        benchmark::DoNotOptimize(engine.events().data());
        engine.cancel(id);
        benchmark::DoNotOptimize(engine.events().data());
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_EnginePassiveLimitCancel);

// a market order that sweeps `levels` price levels (one resting order each). The book for a
// batch of sweeps is built in one paused section, so the timer overhead is amortized away.
static void BM_MarketSweep(benchmark::State& state) {
    const auto levels = static_cast<int>(state.range(0));
    constexpr int kBatch = 1000;
    MatchingEngine engine(1u << 20, 1u << 17);
    OrderId next = 1;
    for (auto _ : state) {
        state.PauseTiming();
        // asks at increasing prices: each market buy eats the next `levels` of them
        for (int l = 1; l <= levels * kBatch; ++l) {
            engine.submit({next++, Side::Sell, OrderType::Limit, TimeInForce::GTC, kMid + l, 10});
        }
        state.ResumeTiming();
        for (int b = 0; b < kBatch; ++b) {
            engine.submit({next++, Side::Buy, OrderType::Market, TimeInForce::IOC, 0,
                           static_cast<Qty>(10 * levels)});
            benchmark::DoNotOptimize(engine.events().size());
        }
    }
    state.SetItemsProcessed(state.iterations() * kBatch);
    state.counters["levels"] = levels;
    state.counters["ns_per_sweep"] = benchmark::Counter(
        static_cast<double>(state.iterations() * kBatch),
        benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_MarketSweep)->Arg(1)->Arg(4)->Arg(16)->Arg(64);

// zero-copy decode of a stream of new orders and cancels
static void BM_ProtocolDecode(benchmark::State& state) {
    const auto stream = replay::synthetic_flow({}, 100'000, 3);
    std::size_t messages = 0;
    for (auto _ : state) {
        std::uint64_t sum = 0;
        for (std::span<const std::byte> in(stream); !in.empty();) {
            const auto r = proto::decode(in);
            sum += r.msg.id();
            in = in.subspan(r.consumed);
            ++messages;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(messages));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(stream.size()));
}
BENCHMARK(BM_ProtocolDecode);

// the whole thing: decode + match + encode events for synthetic Poisson flow
static void BM_SessionSyntheticFlow(benchmark::State& state) {
    const auto stream = replay::synthetic_flow({}, static_cast<std::size_t>(state.range(0)), 5);
    for (auto _ : state) {
        state.PauseTiming();
        auto engine = std::make_unique<MatchingEngine>(1u << 20, 1u << 16);
        proto::Session session(*engine);
        state.ResumeTiming();
        session.feed(stream);
        benchmark::DoNotOptimize(session.output().data());
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SessionSyntheticFlow)->Arg(100'000)->Unit(benchmark::kMillisecond);

// one producer, one consumer thread through the ring, padded vs plain counters
template <bool Padded>
static void BM_SpscRing(benchmark::State& state) {
    constexpr std::uint64_t kItems = 1'000'000;
    for (auto _ : state) {
        auto q = std::make_unique<SpscRing<std::uint64_t, 4096, Padded>>();
        std::thread producer([&] {
            for (std::uint64_t i = 0; i < kItems;) {
                if (q->try_push(i)) ++i;
            }
        });
        std::uint64_t got = 0, v = 0;
        while (got < kItems) {
            if (q->try_pop(v)) ++got;
        }
        producer.join();
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kItems));
}
BENCHMARK(BM_SpscRing<true>)->Name("BM_SpscRing/padded")->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SpscRing<false>)->Name("BM_SpscRing/plain")->UseRealTime()->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
