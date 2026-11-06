#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "matchbook/engine.hpp"
#include "matchbook/protocol.hpp"

namespace matchbook::proto {

/// Glue between a byte stream and the engine: decodes client messages, applies them, and
/// encodes the engine's events into an output buffer.
///
/// Input may arrive in arbitrary chunks (like reads from a TCP socket); a message split across
/// two `feed` calls is buffered until it's complete. A malformed message stops the session:
/// with fixed-size framing there's no safe way to find the next message boundary.
class Session {
public:
    explicit Session(MatchingEngine& engine) : engine_(engine) {
        pending_.reserve(64);
        out_.reserve(1u << 16);
    }

    /// Processes as many complete messages as `bytes` (plus leftovers) contain.
    /// Returns Ok, or the first decode error.
    DecodeStatus feed(std::span<const std::byte> bytes);

    /// Encoded events produced so far; the caller drains it with `clear_output`.
    [[nodiscard]] std::span<const std::byte> output() const noexcept { return out_; }
    void clear_output() noexcept { out_.clear(); }

    [[nodiscard]] std::uint64_t messages_in() const noexcept { return in_count_; }
    [[nodiscard]] std::uint32_t next_out_seq() const noexcept { return out_seq_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return pending_.size(); }

private:
    void apply(const MessageView& m);
    std::size_t consume(std::span<const std::byte> buf, DecodeStatus& status);

    MatchingEngine& engine_;
    std::vector<std::byte> pending_;  // tail of a message split across feeds
    std::vector<std::byte> out_;
    std::uint64_t in_count_ = 0;
    std::uint32_t out_seq_ = 1;
    DecodeStatus failed_ = DecodeStatus::Ok;
};

}  // namespace matchbook::proto
