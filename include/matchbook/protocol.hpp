#pragma once

// Compact binary wire protocol, in the spirit of exchange protocols like NASDAQ OUCH/ITCH:
// fixed-size little-endian messages behind an 8-byte header, so a decoder never has to scan
// for delimiters and every field sits at a known offset.
//
//   header   0  u16 length   whole message incl. header
//            2  u8  type     ASCII tag, see MsgType
//            3  u8  version  kProtocolVersion
//            4  u32 seq      sender's sequence number
//
// Decoding is zero-copy: `MessageView` points into the receive buffer and reads each field
// with an unaligned little-endian load when it's asked for. Nothing on the decode path throws
// or allocates; malformed input comes back as a `DecodeStatus`.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "matchbook/engine.hpp"
#include "matchbook/types.hpp"

namespace matchbook::proto {

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kHeaderSize = 8;

enum class MsgType : std::uint8_t {
    // client -> engine
    NewOrder = 'N',
    Cancel = 'X',
    Modify = 'M',
    // engine -> client
    Trade = 'T',
    Rested = 'R',
    Cancelled = 'C',
    Rejected = 'J',
};

/// Wire size of each message type, 0 for unknown types.
constexpr std::size_t message_size(MsgType t) noexcept {
    switch (t) {
        case MsgType::NewOrder:
            return 40;  // header + id + price + qty + side/type/tif + 5 pad
        case MsgType::Cancel:
            return 16;  // header + id
        case MsgType::Modify:
            return 32;  // header + id + price + qty
        case MsgType::Trade:
            return 40;  // header + maker + taker + price + qty
        case MsgType::Rested:
            return 32;  // header + id + price + qty
        case MsgType::Cancelled:
            return 32;  // header + id + price + qty (same layout as Rested)
        case MsgType::Rejected:
            return 24;  // header + id + reason + 7 pad
    }
    return 0;
}

// ---- little-endian loads / stores ------------------------------------------------------------

template <class T>
T load_le(const std::byte* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));  // compiles to a single mov; p needn't be aligned
    if constexpr (std::endian::native == std::endian::big) {
        // no big-endian target in CI, but keep the wire format honest
        auto u = std::bit_cast<std::make_unsigned_t<T>>(v);
        std::make_unsigned_t<T> r = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) r = (r << 8) | ((u >> (8 * i)) & 0xFF);
        v = std::bit_cast<T>(r);
    }
    return v;
}

template <class T>
void store_le(std::byte* p, T v) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        auto u = std::bit_cast<std::make_unsigned_t<T>>(v);
        std::make_unsigned_t<T> r = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) r = (r << 8) | ((u >> (8 * i)) & 0xFF);
        v = std::bit_cast<T>(r);
    }
    std::memcpy(p, &v, sizeof(T));
}

// ---- decoding ---------------------------------------------------------------------------------

enum class DecodeStatus : std::uint8_t {
    Ok,
    NeedMore,      // buffer ends in the middle of a message: wait for more bytes
    UnknownType,
    BadLength,     // length field doesn't match the type's fixed size
    BadVersion,
    BadField,      // e.g. side byte that isn't 0 or 1
};

const char* to_string(DecodeStatus s) noexcept;

/// Read-only view of one complete, validated message inside a buffer.
class MessageView {
public:
    MessageView() = default;
    explicit MessageView(const std::byte* p) noexcept : p_(p) {}

    [[nodiscard]] MsgType type() const noexcept { return static_cast<MsgType>(p_[2]); }
    [[nodiscard]] std::uint16_t length() const noexcept { return load_le<std::uint16_t>(p_); }
    [[nodiscard]] std::uint32_t seq() const noexcept { return load_le<std::uint32_t>(p_ + 4); }

    // field at byte offset 8 is the order id for every message type except Trade (maker id)
    [[nodiscard]] OrderId id() const noexcept { return load_le<OrderId>(p_ + 8); }

    /// NewOrder / Modify / Rested / Cancelled: price at 16, qty at 24.
    [[nodiscard]] Price price() const noexcept { return load_le<Price>(p_ + 16); }
    [[nodiscard]] Qty qty() const noexcept { return load_le<Qty>(p_ + 24); }
    [[nodiscard]] Side side() const noexcept { return static_cast<Side>(p_[32]); }
    [[nodiscard]] OrderType order_type() const noexcept { return static_cast<OrderType>(p_[33]); }
    [[nodiscard]] TimeInForce tif() const noexcept { return static_cast<TimeInForce>(p_[34]); }

    /// Trade: maker at 8, taker at 16, price at 24, qty at 32.
    [[nodiscard]] OrderId taker_id() const noexcept { return load_le<OrderId>(p_ + 16); }
    [[nodiscard]] Price trade_price() const noexcept { return load_le<Price>(p_ + 24); }
    [[nodiscard]] Qty trade_qty() const noexcept { return load_le<Qty>(p_ + 32); }
    /// Rejected: reason at 16.
    [[nodiscard]] RejectReason reason() const noexcept {
        return static_cast<RejectReason>(p_[16]);
    }

    [[nodiscard]] NewOrder to_new_order() const noexcept {
        return {id(), side(), order_type(), tif(), price(), qty()};
    }
    /// The engine event this message encodes (Trade / Rested / Cancelled / Rejected).
    [[nodiscard]] Event to_event() const noexcept;

private:
    const std::byte* p_ = nullptr;
};

struct DecodeResult {
    DecodeStatus status;
    MessageView msg;       // valid only when status == Ok
    std::size_t consumed;  // bytes to drop: message size on Ok, 0 on NeedMore / errors
};

/// Validates the message at the start of `buf`. Never reads past `buf.size()`.
DecodeResult decode(std::span<const std::byte> buf) noexcept;

// ---- encoding ---------------------------------------------------------------------------------

/// Each encoder writes one message at `out` and returns its size, or 0 if `out` is too small.
std::size_t encode_new_order(std::span<std::byte> out, const NewOrder& o, std::uint32_t seq);
std::size_t encode_cancel(std::span<std::byte> out, OrderId id, std::uint32_t seq);
std::size_t encode_modify(std::span<std::byte> out, OrderId id, Price price, Qty qty,
                          std::uint32_t seq);
std::size_t encode_event(std::span<std::byte> out, const Event& e, std::uint32_t seq);

}  // namespace matchbook::proto
