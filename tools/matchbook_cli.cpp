// Small command-line entry point: build info and a quick protocol throughput check. Replay and
// proper Google Benchmark suites come with the later phases.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "matchbook/pipeline.hpp"
#include "matchbook/protocol.hpp"
#include "matchbook/session.hpp"
#include "matchbook/spsc.hpp"
#include "matchbook/types.hpp"
#include "matchbook/version.hpp"

namespace {

void print_info() {
    std::printf("matchbook %s\n", matchbook::kVersion);
#if defined(__clang__)
    std::printf("compiler: clang %d.%d\n", __clang_major__, __clang_minor__);
#elif defined(__GNUC__)
    std::printf("compiler: gcc %d.%d\n", __GNUC__, __GNUC_MINOR__);
#elif defined(_MSC_VER)
    std::printf("compiler: msvc %d\n", _MSC_VER);
#endif
    std::printf("C++ standard: %ld\n", static_cast<long>(__cplusplus));
    // sizes matter here: these live in the hot path and get copied around a lot
    std::printf("sizeof(Price)=%zu sizeof(Qty)=%zu sizeof(OrderId)=%zu\n", sizeof(matchbook::Price),
                sizeof(matchbook::Qty), sizeof(matchbook::OrderId));
}

/// Random limit orders around a mid price plus ~10% cancels of earlier ids, already encoded.
std::vector<std::byte> make_stream(std::size_t n, std::uint32_t seed) {
    using namespace matchbook;
    std::mt19937_64 rng(seed);
    std::vector<std::byte> out(n * 40);
    std::size_t at = 0;
    for (std::size_t i = 0; i < n; ++i) {
        std::span<std::byte> dst(out.data() + at, out.size() - at);
        const auto seq = static_cast<std::uint32_t>(i);
        if (i > 0 && rng() % 10 == 0) {
            at += proto::encode_cancel(dst, 1 + rng() % i, seq);
        } else {
            const NewOrder o{i + 1,
                             rng() % 2 ? Side::Buy : Side::Sell,
                             OrderType::Limit,
                             rng() % 8 == 0 ? TimeInForce::IOC : TimeInForce::GTC,
                             static_cast<Price>(9'950 + rng() % 101),
                             static_cast<Qty>(1 + rng() % 100)};
            at += proto::encode_new_order(dst, o, seq);
        }
    }
    out.resize(at);
    return out;
}

int bench_protocol(std::size_t n) {
    using namespace matchbook;
    using clock = std::chrono::steady_clock;
    const auto stream = make_stream(n, 42);

    // 1) decode only: walk the buffer and touch every field the engine would read
    auto t0 = clock::now();
    std::uint64_t checksum = 0;
    std::span<const std::byte> in(stream);
    while (!in.empty()) {
        const auto r = proto::decode(in);
        checksum += r.msg.id() + static_cast<std::uint64_t>(r.msg.qty());
        in = in.subspan(r.consumed);
    }
    const double decode_ns = std::chrono::duration<double, std::nano>(clock::now() - t0).count();

    // 2) decode + match + encode the outbound events
    MatchingEngine engine(n + 16);
    proto::Session session(engine);
    t0 = clock::now();
    const auto status = session.feed(stream);
    const double full_ns = std::chrono::duration<double, std::nano>(clock::now() - t0).count();
    if (status != proto::DecodeStatus::Ok) {
        std::fprintf(stderr, "decode error: %s\n", proto::to_string(status));
        return 1;
    }

    std::printf("%zu messages (%.1f MB), %llu trades, %zu bytes out (checksum %llu)\n", n,
                static_cast<double>(stream.size()) / 1e6,
                static_cast<unsigned long long>(engine.trade_count()), session.output().size(),
                static_cast<unsigned long long>(checksum % 1000));
    const auto msgs = static_cast<double>(n);
    std::printf("decode only:             %5.1f ns/msg  (%.1f M msg/s)\n", decode_ns / msgs,
                msgs / decode_ns * 1e3);
    std::printf("decode + match + encode: %5.1f ns/msg  (%.1f M msg/s)\n", full_ns / msgs,
                msgs / full_ns * 1e3);
    return 0;
}

template <bool Padded>
double ring_mops(std::uint64_t n) {
    auto q = std::make_unique<matchbook::SpscRing<std::uint64_t, 4096, Padded>>();
    const auto t0 = std::chrono::steady_clock::now();
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < n;) {
            if (q->try_push(i)) ++i;
        }
    });
    std::uint64_t got = 0, v = 0, sum = 0;
    while (got < n) {
        if (q->try_pop(v)) {
            sum += v;
            ++got;
        }
    }
    producer.join();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (sum != n * (n - 1) / 2) std::fprintf(stderr, "lost items!\n");
    return static_cast<double>(n) / s / 1e6;
}

int bench_queue(std::size_t n) {
    using namespace matchbook;
    // the ring alone, best of 3 (thread start-up and scheduling make single runs noisy)
    double plain = 0, padded = 0;
    for (int r = 0; r < 3; ++r) {
        plain = std::max(plain, ring_mops<false>(20'000'000));
        padded = std::max(padded, ring_mops<true>(20'000'000));
    }
    std::printf("spsc ring, 2 threads:  plain %.1f M ops/s, padded + cached indices %.1f M ops/s "
                "(%.1fx)\n",
                plain, padded, padded / plain);

    const auto stream = make_stream(n, 42);
    MatchingEngine engine(n + 16);
    proto::Session session(engine);
    const auto t0 = std::chrono::steady_clock::now();
    session.feed(stream);
    const double one = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    Pipeline pipeline(n + 16);
    const auto r = pipeline.run(stream, false);
    const auto msgs = static_cast<double>(n);
    std::printf("single thread (Session): %.1f M msg/s\n", msgs / one / 1e6);
    std::printf("3-thread pipeline:       %.1f M msg/s, %llu events\n", msgs / r.seconds / 1e6,
                static_cast<unsigned long long>(r.events));
    std::printf("  (unpaced, the rings sit full: latency is just queue depth, p50 %.1f ms)\n",
                percentile(r.latency_ns, 50) / 1e6);

    // latency at offered loads the pipeline can sustain
    for (bool pinned : {false, true}) {
        for (double rate : {1e6, 3e6, 5e6}) {
            Pipeline paced(n + 16);
            // cores 2, 4, 6: separate physical cores (logical CPUs 2k / 2k+1 are SMT siblings)
            if (pinned) paced.pin_threads({2, 4, 6});
            const auto pr = paced.run(stream, false, rate);
            std::printf("%s paced %.0fM msg/s: p50 %u ns, p99 %u ns, p99.9 %u ns\n",
                        pinned ? "pinned  " : "unpinned", rate / 1e6,
                        percentile(pr.latency_ns, 50), percentile(pr.latency_ns, 99),
                        percentile(pr.latency_ns, 99.9));
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "bench-queue") == 0) {
        const std::size_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2'000'000;
        return bench_queue(n);
    }
    if (argc > 1 && std::strcmp(argv[1], "--info") == 0) {
        print_info();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "bench-protocol") == 0) {
        const std::size_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2'000'000;
        return bench_protocol(n);
    }
    std::printf("usage: matchbook --info\n       matchbook bench-protocol [messages]\n       matchbook bench-queue [messages]\n");
    return argc > 1 ? 1 : 0;
}
