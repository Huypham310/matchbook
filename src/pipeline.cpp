#include "matchbook/pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

#include "matchbook/protocol.hpp"
#include "matchbook/spsc.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define MATCHBOOK_HAVE_TSC 1
#endif
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace matchbook {

namespace {

// ---- cheap timestamps ---------------------------------------------------------------------------
// steady_clock on Windows is QueryPerformanceCounter: ~100 ns resolution, too coarse for a stage
// that takes ~80 ns. The TSC ticks at a constant rate on every CPU from the last decade, so read it
// directly and convert with a rate measured once against steady_clock.

inline std::uint64_t ticks() noexcept {
#ifdef MATCHBOOK_HAVE_TSC
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

double ns_per_tick() {
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

/// Pins the calling thread to one logical CPU (no-op if `cpu < 0` or unsupported).
void pin_to(int cpu) {
    if (cpu < 0) return;
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

// ---- what travels between the stages ------------------------------------------------------------

enum class CmdKind : std::uint8_t { New, Cancel, Modify, Stop };

struct Command {
    CmdKind kind = CmdKind::Stop;
    NewOrder order{};  // New; Cancel / Modify use order.id (+ price / qty for Modify)
    std::uint64_t t_in = 0;
};

struct OutEvent {
    Event event{EventType::Trade};
    std::uint64_t t_in = 0;
    bool last = false;  // last event produced by its command: closes the latency measurement
    bool stop = false;
};

// 64K commands / events of headroom: a stage can fall behind for a while without blocking
constexpr std::size_t kRing = 1u << 16;
using CommandRing = SpscRing<Command, kRing>;
using EventRing = SpscRing<OutEvent, kRing>;

template <class Ring, class T>
void push_spin(Ring& ring, const T& v) {
    while (!ring.try_push(v)) cpu_relax();
}

}  // namespace

std::uint32_t percentile(std::vector<std::uint32_t> v, double p) {
    if (v.empty()) return 0;
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(v.size())));
    const std::size_t k = std::clamp<std::size_t>(rank, 1, v.size()) - 1;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

Pipeline::Result Pipeline::run(std::span<const std::byte> stream, bool keep_output, double rate) {
    // rings are ~MBs each: heap, not the stack
    auto commands = std::make_unique<CommandRing>();
    auto events = std::make_unique<EventRing>();
    Result result;
    const double tick_ns = ns_per_tick();
    result.latency_ns.reserve(stream.size() / proto::message_size(proto::MsgType::Cancel));

    std::thread engine_thread([&, max_orders = max_orders_, max_levels = max_levels_] {
        pin_to(cpus_[1]);
        MatchingEngine engine(max_orders, max_levels);
        Command cmd;
        while (true) {
            if (!commands->try_pop(cmd)) {
                cpu_relax();
                continue;
            }
            if (cmd.kind == CmdKind::Stop) break;
            switch (cmd.kind) {
                case CmdKind::New:
                    engine.submit(cmd.order);
                    break;
                case CmdKind::Cancel:
                    engine.cancel(cmd.order.id);
                    break;
                default:
                    engine.modify(cmd.order.id, cmd.order.price, cmd.order.qty);
                    break;
            }
            const auto& evs = engine.events();
            for (std::size_t i = 0; i < evs.size(); ++i) {
                push_spin(*events, OutEvent{evs[i], cmd.t_in, i + 1 == evs.size(), false});
            }
        }
        OutEvent stop;
        stop.stop = true;
        push_spin(*events, stop);
    });

    std::thread publisher([&] {
        pin_to(cpus_[2]);
        std::uint32_t seq = 1;
        if (keep_output) result.output.reserve(stream.size() * 2);
        std::byte buf[64];
        OutEvent ev;
        while (true) {
            if (!events->try_pop(ev)) {
                cpu_relax();
                continue;
            }
            if (ev.stop) break;
            const std::size_t n = proto::encode_event(buf, ev.event, seq++);
            if (keep_output) result.output.insert(result.output.end(), buf, buf + n);
            ++result.events;
            if (ev.event.type == EventType::Trade) ++result.trades;
            if (ev.last) {
                const double ns = static_cast<double>(ticks() - ev.t_in) * tick_ns;
                result.latency_ns.push_back(static_cast<std::uint32_t>(std::min(ns, 4e9)));
            }
        }
    });

    // gateway gets its own thread too, so pinning it doesn't change the caller's affinity
    const auto start = std::chrono::steady_clock::now();
    std::thread gateway([&] {
    pin_to(cpus_[0]);
    const double ticks_per_msg = rate > 0 ? 1e9 / rate / tick_ns : 0.0;
    const std::uint64_t t_first = ticks();
    std::uint64_t sent = 0;
    while (!stream.empty()) {
        const auto r = proto::decode(stream);
        if (r.status != proto::DecodeStatus::Ok) break;
        if (ticks_per_msg > 0) {
            // open-loop pacing: message i is due at t_first + i / rate, whatever happened before
            const auto due = t_first + static_cast<std::uint64_t>(static_cast<double>(sent) *
                                                                  ticks_per_msg);
            while (ticks() < due) cpu_relax();
        }
        ++sent;
        Command cmd;
        cmd.t_in = ticks();
        switch (r.msg.type()) {
            case proto::MsgType::NewOrder:
                cmd.kind = CmdKind::New;
                cmd.order = r.msg.to_new_order();
                break;
            case proto::MsgType::Cancel:
                cmd.kind = CmdKind::Cancel;
                cmd.order.id = r.msg.id();
                break;
            case proto::MsgType::Modify:
                cmd.kind = CmdKind::Modify;
                cmd.order.id = r.msg.id();
                cmd.order.price = r.msg.price();
                cmd.order.qty = r.msg.qty();
                break;
            default:
                stream = stream.subspan(r.consumed);
                continue;  // outbound types are ignored, same as Session
        }
        push_spin(*commands, cmd);
        ++result.messages;
        stream = stream.subspan(r.consumed);
    }
    push_spin(*commands, Command{});  // default kind is Stop
    });
    gateway.join();
    engine_thread.join();
    publisher.join();
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

}  // namespace matchbook
