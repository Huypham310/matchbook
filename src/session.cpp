#include "matchbook/session.hpp"

#include <algorithm>

namespace matchbook::proto {

void Session::apply(const MessageView& m) {
    switch (m.type()) {
        case MsgType::NewOrder:
            engine_.submit(m.to_new_order());
            break;
        case MsgType::Cancel:
            engine_.cancel(m.id());
            break;
        case MsgType::Modify:
            engine_.modify(m.id(), m.price(), m.qty());
            break;
        default:
            // outbound types sent to the engine are ignored rather than fatal
            return;
    }
    ++in_count_;
    for (const Event& e : engine_.events()) {
        const std::size_t at = out_.size();
        out_.resize(at + message_size(MsgType::Trade));  // largest outbound message
        const std::size_t n = encode_event({out_.data() + at, out_.size() - at}, e, out_seq_++);
        out_.resize(at + n);
    }
}

std::size_t Session::consume(std::span<const std::byte> buf, DecodeStatus& status) {
    std::size_t used = 0;
    while (true) {
        const DecodeResult r = decode(buf.subspan(used));
        if (r.status == DecodeStatus::NeedMore) break;
        if (r.status != DecodeStatus::Ok) {
            status = r.status;
            break;
        }
        apply(r.msg);
        used += r.consumed;
    }
    return used;
}

DecodeStatus Session::feed(std::span<const std::byte> bytes) {
    if (failed_ != DecodeStatus::Ok) return failed_;
    DecodeStatus status = DecodeStatus::Ok;

    if (!pending_.empty()) {
        // finish the split message first: copy just enough bytes to complete it
        const std::size_t before = pending_.size();
        const std::size_t want = std::min<std::size_t>(64 - before, bytes.size());
        pending_.insert(pending_.end(), bytes.begin(), bytes.begin() + want);
        const std::size_t used = consume(pending_, status);
        if (status != DecodeStatus::Ok) return failed_ = status;
        if (used == 0) {
            // still incomplete (we copied all `bytes` and it wasn't enough)
            pending_.resize(before + want);
            return DecodeStatus::Ok;
        }
        // the completed message took `used - before` bytes from the new chunk
        bytes = bytes.subspan(used - before);
        pending_.clear();
    }

    const std::size_t used = consume(bytes, status);
    if (status != DecodeStatus::Ok) return failed_ = status;
    // whatever is left is the start of a message (< largest message size)
    pending_.assign(bytes.begin() + used, bytes.end());
    return DecodeStatus::Ok;
}

}  // namespace matchbook::proto
