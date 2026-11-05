// Small command-line entry point: build info and a quick protocol throughput check. Replay and
// proper Google Benchmark suites come with the later phases.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "matchbook/protocol.hpp"
#include "matchbook/session.hpp"
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

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--info") == 0) {
        print_info();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "bench-protocol") == 0) {
        const std::size_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2'000'000;
        return bench_protocol(n);
    }
    std::printf("usage: matchbook --info\n       matchbook bench-protocol [messages]\n");
    return argc > 1 ? 1 : 0;
}
