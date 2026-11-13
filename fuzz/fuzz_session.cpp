// libFuzzer target: arbitrary bytes into a Session, split into arbitrary chunks.
//
//   cmake -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DMATCHBOOK_FUZZ=ON
//   cmake --build build/fuzz && ./build/fuzz/fuzz/fuzz_session -max_total_time=60
//
// The first byte picks the chunk size, so the fuzzer also explores messages split across
// feeds. After every chunk the book must satisfy its invariants and must not be crossed;
// ASan / UBSan catch everything else (out-of-bounds reads in the decoder, overflow, ...).

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "matchbook/engine.hpp"
#include "matchbook/session.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const std::size_t chunk = 1 + data[0] % 64;
    std::span<const std::byte> in(reinterpret_cast<const std::byte*>(data + 1), size - 1);

    // small pools: a random stream that fills them must get BookFull rejects, not a crash
    matchbook::MatchingEngine engine(256, 64);
    matchbook::proto::Session session(engine);
    while (!in.empty()) {
        const std::size_t n = std::min(chunk, in.size());
        if (session.feed(in.first(n)) != matchbook::proto::DecodeStatus::Ok) break;
        in = in.subspan(n);
        engine.book().check_invariants();
        const auto bid = engine.book().best_bid();
        const auto ask = engine.book().best_ask();
        if (bid && ask && *bid >= *ask) std::abort();  // crossed book
        session.clear_output();
    }
    return 0;
}
