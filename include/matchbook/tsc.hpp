#pragma once

// Cheap timestamps for latency measurements. steady_clock on Windows is QueryPerformanceCounter
// with ~100 ns resolution, too coarse for work that takes ~80 ns, so read the TSC directly
// (constant-rate on every x86 CPU of the last decade) and convert with a measured rate.

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define MATCHBOOK_HAVE_TSC 1
#endif

namespace matchbook {

inline std::uint64_t ticks() noexcept {
#ifdef MATCHBOOK_HAVE_TSC
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

/// Nanoseconds per tick, measured once against steady_clock over ~20 ms.
inline double ns_per_tick() {
#ifdef MATCHBOOK_HAVE_TSC
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::uint64_t c0 = ticks();
    while (clock::now() - t0 < std::chrono::milliseconds(20)) {
    }
    const double ns = std::chrono::duration<double, std::nano>(clock::now() - t0).count();
    return ns / static_cast<double>(ticks() - c0);
#else
    return 1e9 * std::chrono::steady_clock::period::num / std::chrono::steady_clock::period::den;
#endif
}

inline void cpu_relax() noexcept {
#ifdef MATCHBOOK_HAVE_TSC
    _mm_pause();  // tells the core we're spinning: saves power, frees the sibling hyperthread
#else
    std::this_thread::yield();
#endif
}

}  // namespace matchbook
