#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <random>
#include <vector>

#include "matchbook/protocol.hpp"
#include "matchbook/session.hpp"

using namespace matchbook;
using namespace matchbook::proto;

namespace {

std::vector<std::byte> bytes_of(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    for (int b : v) out.push_back(static_cast<std::byte>(b));
    return out;
}

}  // namespace

TEST(Protocol, MessageSizesAreMultiplesOfEight) {
    for (auto t : {MsgType::NewOrder, MsgType::Cancel, MsgType::Modify, MsgType::Trade,
                   MsgType::Rested, MsgType::Cancelled, MsgType::Rejected}) {
        EXPECT_EQ(message_size(t) % 8, 0u);  // keeps every field naturally aligned in a stream
    }
    EXPECT_EQ(message_size(static_cast<MsgType>('?')), 0u);
}

TEST(Protocol, NewOrderWireLayoutIsPinned) {
    // golden bytes: changing the layout must be a deliberate, versioned decision
    std::array<std::byte, 40> buf{};
    const NewOrder o{0x0102030405060708ULL, Side::Sell, OrderType::Limit, TimeInForce::IOC, 1000, 7};
    ASSERT_EQ(encode_new_order(buf, o, 0xAABBCCDD), 40u);
    const auto want = bytes_of({40, 0, 'N', 1, 0xDD, 0xCC, 0xBB, 0xAA,  // header
                                8,  7, 6,   5, 4,    3,    2,    1,     // id
                                0xE8, 3, 0, 0, 0, 0, 0, 0,              // price 1000
                                7,  0, 0,   0, 0,    0,    0,    0,     // qty
                                1,  0, 1,   0, 0,    0,    0,    0});   // sell, limit, ioc
    EXPECT_TRUE(std::equal(buf.begin(), buf.end(), want.begin()));
}

TEST(Protocol, ClientMessagesRoundTrip) {
    std::array<std::byte, 128> buf{};
    const NewOrder o{42, Side::Buy, OrderType::Market, TimeInForce::FOK, -5, 1'000'000};
    std::size_t n = encode_new_order(buf, o, 9);
    auto r = decode(std::span(buf).first(n));
    ASSERT_EQ(r.status, DecodeStatus::Ok);
    EXPECT_EQ(r.consumed, 40u);
    EXPECT_EQ(r.msg.seq(), 9u);
    const NewOrder back = r.msg.to_new_order();
    EXPECT_EQ(back.id, 42u);
    EXPECT_EQ(back.side, Side::Buy);
    EXPECT_EQ(back.type, OrderType::Market);
    EXPECT_EQ(back.tif, TimeInForce::FOK);
    EXPECT_EQ(back.price, -5);  // negative prices survive (spreads, some futures)
    EXPECT_EQ(back.qty, 1'000'000);

    n = encode_cancel(buf, 77, 10);
    r = decode(std::span(buf).first(n));
    ASSERT_EQ(r.status, DecodeStatus::Ok);
    EXPECT_EQ(r.msg.type(), MsgType::Cancel);
    EXPECT_EQ(r.msg.id(), 77u);

    n = encode_modify(buf, 78, 101, 3, 11);
    r = decode(std::span(buf).first(n));
    ASSERT_EQ(r.status, DecodeStatus::Ok);
    EXPECT_EQ(r.msg.price(), 101);
    EXPECT_EQ(r.msg.qty(), 3);

    EXPECT_EQ(encode_new_order(std::span(buf).first(39), o, 1), 0u);  // too small
}

TEST(Protocol, EventsRoundTrip) {
    const Event events[] = {
        {EventType::Trade, 5, 3, 100, 20, RejectReason::BadQuantity},
        {EventType::Rested, 6, 0, 99, 8, RejectReason::BadQuantity},
        {EventType::Cancelled, 7, 0, 98, 4, RejectReason::BadQuantity},
        {EventType::Rejected, 8, 0, 0, 0, RejectReason::DuplicateId},
    };
    std::array<std::byte, 64> buf{};
    for (const Event& e : events) {
        const std::size_t n = encode_event(buf, e, 1);
        const auto r = decode(std::span(buf).first(n));
        ASSERT_EQ(r.status, DecodeStatus::Ok);
        EXPECT_EQ(r.msg.to_event(), e);
    }
}

TEST(Protocol, RejectsMalformedHeadersWithoutReadingPastTheBuffer) {
    std::array<std::byte, 64> buf{};
    encode_cancel(buf, 1, 1);
    EXPECT_EQ(decode(std::span(buf).first(7)).status, DecodeStatus::NeedMore);
    EXPECT_EQ(decode(std::span(buf).first(15)).status, DecodeStatus::NeedMore);

    auto bad = buf;
    bad[2] = std::byte{'?'};
    EXPECT_EQ(decode(bad).status, DecodeStatus::UnknownType);
    bad = buf;
    bad[3] = std::byte{2};
    EXPECT_EQ(decode(bad).status, DecodeStatus::BadVersion);
    bad = buf;
    bad[0] = std::byte{60};  // claims 60 bytes for a 16-byte cancel
    EXPECT_EQ(decode(bad).status, DecodeStatus::BadLength);

    encode_new_order(buf, {1, Side::Buy, OrderType::Limit, TimeInForce::GTC, 1, 1}, 1);
    bad = buf;
    bad[32] = std::byte{2};  // side must be 0 or 1
    EXPECT_EQ(decode(bad).status, DecodeStatus::BadField);
    bad = buf;
    bad[34] = std::byte{9};  // tif
    EXPECT_EQ(decode(bad).status, DecodeStatus::BadField);
}

TEST(Protocol, RandomBytesNeverDecodeOutOfBounds) {
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> byte(0, 255);
    int ok = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        // exact-size heap buffer, so a read past the end would show up under ASan
        std::vector<std::byte> buf(static_cast<std::size_t>(byte(rng) % 48));
        for (auto& b : buf) b = static_cast<std::byte>(byte(rng));
        if (trial % 2 == 0 && buf.size() >= 4) {
            buf[2] = std::byte{'X'};  // make some of them look like real cancels
            buf[3] = std::byte{1};
            buf[0] = std::byte{16};
            buf[1] = std::byte{0};
        }
        const auto r = decode(buf);
        if (r.status == DecodeStatus::Ok) {
            ++ok;
            EXPECT_LE(r.consumed, buf.size());
        } else {
            EXPECT_EQ(r.consumed, 0u);
        }
    }
    EXPECT_GT(ok, 0);
}

namespace {

std::vector<std::byte> order_stream(int n, std::uint32_t seed, std::vector<NewOrder>* orders) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> price(95, 105), qty(1, 20), kind(0, 9);
    std::vector<std::byte> out(static_cast<std::size_t>(n) * 40);
    std::size_t at = 0;
    for (int i = 0; i < n; ++i) {
        std::span<std::byte> dst(out.data() + at, out.size() - at);
        const auto id = static_cast<OrderId>(i + 1);
        if (kind(rng) == 0 && i > 0) {
            at += encode_cancel(dst, static_cast<OrderId>(1 + rng() % i), static_cast<std::uint32_t>(i));
            if (orders) orders->push_back({0, Side::Buy, OrderType::Limit, TimeInForce::GTC, -1, -1});
        } else {
            NewOrder o{id, rng() % 2 ? Side::Buy : Side::Sell, OrderType::Limit,
                       rng() % 5 == 0 ? TimeInForce::IOC : TimeInForce::GTC, price(rng), qty(rng)};
            at += encode_new_order(dst, o, static_cast<std::uint32_t>(i));
            if (orders) orders->push_back(o);
        }
    }
    out.resize(at);
    return out;
}

std::vector<Event> decode_events(std::span<const std::byte> out) {
    std::vector<Event> events;
    while (!out.empty()) {
        const auto r = decode(out);
        EXPECT_EQ(r.status, DecodeStatus::Ok);
        if (r.status != DecodeStatus::Ok) break;
        events.push_back(r.msg.to_event());
        out = out.subspan(r.consumed);
    }
    return events;
}

}  // namespace

TEST(Session, WireStreamGivesTheSameEventsAsCallingTheEngine) {
    const auto stream = order_stream(3000, 11, nullptr);

    MatchingEngine via_wire;
    Session s(via_wire);
    ASSERT_EQ(s.feed(stream), DecodeStatus::Ok);
    EXPECT_EQ(s.messages_in(), 3000u);
    const auto wire_events = decode_events(s.output());

    // same messages, applied directly
    MatchingEngine direct;
    std::vector<Event> expected;
    std::span<const std::byte> in(stream);
    while (!in.empty()) {
        const auto r = decode(in);
        if (r.msg.type() == MsgType::NewOrder) direct.submit(r.msg.to_new_order());
        else direct.cancel(r.msg.id());
        expected.insert(expected.end(), direct.events().begin(), direct.events().end());
        in = in.subspan(r.consumed);
    }
    EXPECT_EQ(wire_events, expected);
    EXPECT_GT(via_wire.trade_count(), 100u);
    EXPECT_EQ(s.next_out_seq(), expected.size() + 1);  // one outbound seq per event
}

TEST(Session, ArbitraryChunkingDoesNotChangeTheResult) {
    const auto stream = order_stream(500, 3, nullptr);
    MatchingEngine whole_engine;
    Session whole(whole_engine);
    ASSERT_EQ(whole.feed(stream), DecodeStatus::Ok);

    std::mt19937 rng(5);
    for (int run = 0; run < 5; ++run) {
        MatchingEngine e;
        Session s(e);
        std::size_t at = 0;
        while (at < stream.size()) {
            // chunks from 1 byte (worst case) to a few messages, like socket reads
            const std::size_t len = std::min<std::size_t>(1 + rng() % 120, stream.size() - at);
            ASSERT_EQ(s.feed(std::span(stream).subspan(at, len)), DecodeStatus::Ok);
            at += len;
        }
        EXPECT_EQ(s.buffered(), 0u);
        EXPECT_EQ(s.messages_in(), 500u);
        EXPECT_TRUE(std::equal(s.output().begin(), s.output().end(), whole.output().begin(),
                               whole.output().end()));
    }
}

TEST(Session, StopsAtTheFirstBadMessage) {
    auto stream = order_stream(10, 1, nullptr);
    // messages have different sizes, so walk to the 4th one and corrupt its version byte
    std::size_t at = 0;
    for (int i = 0; i < 3; ++i) at += decode(std::span(stream).subspan(at)).consumed;
    stream[at + 3] = std::byte{9};
    MatchingEngine e;
    Session s(e);
    EXPECT_EQ(s.feed(stream), DecodeStatus::BadVersion);
    EXPECT_EQ(s.messages_in(), 3u);
    // and stays stopped
    EXPECT_EQ(s.feed(stream), DecodeStatus::BadVersion);
}
