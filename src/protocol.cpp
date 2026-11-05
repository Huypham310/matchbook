#include "matchbook/protocol.hpp"

#include <algorithm>

namespace matchbook::proto {

namespace {

void write_header(std::byte* p, MsgType type, std::uint32_t seq) noexcept {
    store_le<std::uint16_t>(p, static_cast<std::uint16_t>(message_size(type)));
    p[2] = static_cast<std::byte>(type);
    p[3] = static_cast<std::byte>(kProtocolVersion);
    store_le<std::uint32_t>(p + 4, seq);
}

bool is_known(std::uint8_t t) noexcept {
    return message_size(static_cast<MsgType>(t)) != 0;
}

}  // namespace

const char* to_string(DecodeStatus s) noexcept {
    switch (s) {
        case DecodeStatus::Ok:
            return "ok";
        case DecodeStatus::NeedMore:
            return "need more bytes";
        case DecodeStatus::UnknownType:
            return "unknown message type";
        case DecodeStatus::BadLength:
            return "bad length";
        case DecodeStatus::BadVersion:
            return "bad version";
        case DecodeStatus::BadField:
            return "bad field";
    }
    return "?";
}

DecodeResult decode(std::span<const std::byte> buf) noexcept {
    if (buf.size() < kHeaderSize) return {DecodeStatus::NeedMore, {}, 0};
    const std::byte* p = buf.data();
    const auto type = std::to_integer<std::uint8_t>(p[2]);
    // validate the header before trusting `length`: a garbage length must not make us wait
    // forever for bytes that will never come
    if (!is_known(type)) return {DecodeStatus::UnknownType, {}, 0};
    if (std::to_integer<std::uint8_t>(p[3]) != kProtocolVersion) {
        return {DecodeStatus::BadVersion, {}, 0};
    }
    const std::size_t size = message_size(static_cast<MsgType>(type));
    if (load_le<std::uint16_t>(p) != size) return {DecodeStatus::BadLength, {}, 0};
    if (buf.size() < size) return {DecodeStatus::NeedMore, {}, 0};

    MessageView v(p);
    if (v.type() == MsgType::NewOrder) {
        // enums arrive as raw bytes; anything out of range would be UB once cast and switched on
        const auto side = std::to_integer<std::uint8_t>(p[32]);
        const auto otype = std::to_integer<std::uint8_t>(p[33]);
        const auto tif = std::to_integer<std::uint8_t>(p[34]);
        if (side > 1 || otype > 1 || tif > 2) return {DecodeStatus::BadField, {}, 0};
    } else if (v.type() == MsgType::Rejected) {
        if (std::to_integer<std::uint8_t>(p[16]) > 3) return {DecodeStatus::BadField, {}, 0};
    }
    return {DecodeStatus::Ok, v, size};
}

Event MessageView::to_event() const noexcept {
    switch (type()) {
        case MsgType::Trade:
            return {EventType::Trade, taker_id(), id(), trade_price(), trade_qty(),
                    RejectReason::BadQuantity};
        case MsgType::Rested:
            return {EventType::Rested, id(), 0, price(), qty(), RejectReason::BadQuantity};
        case MsgType::Cancelled:
            return {EventType::Cancelled, id(), 0, price(), qty(), RejectReason::BadQuantity};
        default:
            return {EventType::Rejected, id(), 0, 0, 0, reason()};
    }
}

std::size_t encode_new_order(std::span<std::byte> out, const NewOrder& o, std::uint32_t seq) {
    constexpr std::size_t n = message_size(MsgType::NewOrder);
    if (out.size() < n) return 0;
    std::byte* p = out.data();
    write_header(p, MsgType::NewOrder, seq);
    store_le<OrderId>(p + 8, o.id);
    store_le<Price>(p + 16, o.price);
    store_le<Qty>(p + 24, o.qty);
    p[32] = static_cast<std::byte>(o.side);
    p[33] = static_cast<std::byte>(o.type);
    p[34] = static_cast<std::byte>(o.tif);
    std::fill(p + 35, p + n, std::byte{0});  // padding keeps every message 8-byte sized
    return n;
}

std::size_t encode_cancel(std::span<std::byte> out, OrderId id, std::uint32_t seq) {
    constexpr std::size_t n = message_size(MsgType::Cancel);
    if (out.size() < n) return 0;
    write_header(out.data(), MsgType::Cancel, seq);
    store_le<OrderId>(out.data() + 8, id);
    return n;
}

std::size_t encode_modify(std::span<std::byte> out, OrderId id, Price price, Qty qty,
                          std::uint32_t seq) {
    constexpr std::size_t n = message_size(MsgType::Modify);
    if (out.size() < n) return 0;
    std::byte* p = out.data();
    write_header(p, MsgType::Modify, seq);
    store_le<OrderId>(p + 8, id);
    store_le<Price>(p + 16, price);
    store_le<Qty>(p + 24, qty);
    return n;
}

std::size_t encode_event(std::span<std::byte> out, const Event& e, std::uint32_t seq) {
    MsgType type{};
    switch (e.type) {
        case EventType::Trade:
            type = MsgType::Trade;
            break;
        case EventType::Rested:
            type = MsgType::Rested;
            break;
        case EventType::Cancelled:
            type = MsgType::Cancelled;
            break;
        case EventType::Rejected:
            type = MsgType::Rejected;
            break;
    }
    const std::size_t n = message_size(type);
    if (out.size() < n) return 0;
    std::byte* p = out.data();
    std::fill(p, p + n, std::byte{0});
    write_header(p, type, seq);
    switch (e.type) {
        case EventType::Trade:
            store_le<OrderId>(p + 8, e.maker_id);
            store_le<OrderId>(p + 16, e.id);
            store_le<Price>(p + 24, e.price);
            store_le<Qty>(p + 32, e.qty);
            break;
        case EventType::Rested:
        case EventType::Cancelled:
            store_le<OrderId>(p + 8, e.id);
            store_le<Price>(p + 16, e.price);
            store_le<Qty>(p + 24, e.qty);
            break;
        case EventType::Rejected:
            store_le<OrderId>(p + 8, e.id);
            p[16] = static_cast<std::byte>(e.reason);
            break;
    }
    return n;
}

}  // namespace matchbook::proto
