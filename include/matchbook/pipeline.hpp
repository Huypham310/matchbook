#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "matchbook/engine.hpp"

namespace matchbook {

/// Three-stage, three-thread pipeline connected by SPSC rings:
///
///   gateway (caller's thread)  --commands-->  engine thread  --events-->  publisher thread
///   decodes wire messages                      matches           encodes outbound messages
///
/// Each stage owns its data: the engine is touched only by the engine thread, the output
/// buffer only by the publisher, so nothing needs a lock. The rings are the only shared state.
/// The encoded output is byte-for-byte what a single-threaded proto::Session produces for the
/// same input (a test checks this).
class Pipeline {
public:
    struct Result {
        std::uint64_t messages = 0;
        std::uint64_t events = 0;
        std::uint64_t trades = 0;
        double seconds = 0.0;
        std::vector<std::byte> output;
        /// Gateway-decode -> publisher-has-the-last-event, per message, in nanoseconds.
        std::vector<std::uint32_t> latency_ns;
    };

    explicit Pipeline(std::size_t max_orders = 1u << 20, std::size_t max_levels = 1u << 16)
        : max_orders_(max_orders), max_levels_(max_levels) {}

    /// Runs the whole stream through the pipeline and joins the worker threads.
    /// Stops at the first malformed message, like Session.
    ///
    /// `rate` (messages per second, 0 = as fast as possible) paces the gateway. Unpaced, the
    /// rings fill up and the latency numbers only measure queue depth; latency has to be
    /// measured at an offered load the pipeline can actually sustain.
    Result run(std::span<const std::byte> stream, bool keep_output = true, double rate = 0.0);

    /// Logical CPUs for gateway / engine / publisher (-1 = let the OS schedule). Pick separate
    /// physical cores: two stages on SMT siblings fight over the same execution units.
    void pin_threads(std::array<int, 3> cpus) noexcept { cpus_ = cpus; }

private:
    std::size_t max_orders_;
    std::size_t max_levels_;
    std::array<int, 3> cpus_{-1, -1, -1};
};

/// p-th percentile (0-100) of a latency sample, nearest-rank. Sorts a copy.
std::uint32_t percentile(std::vector<std::uint32_t> v, double p);

}  // namespace matchbook
