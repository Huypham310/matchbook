#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

namespace matchbook {

// 64 bytes on every x86-64 and most ARM cores. std::hardware_destructive_interference_size
// exists but GCC warns that its value may change between compiler flags, so pin it.
inline constexpr std::size_t kCacheLine = 64;

/// Bounded single-producer / single-consumer queue, lock-free and wait-free.
///
/// One thread may call try_push, one other thread may call try_pop. Capacity is a power of two
/// so the slot index is `counter & (N - 1)`; the counters only ever grow (64-bit, they won't
/// wrap in practice), so "full" is `tail - head == N` with no wasted slot.
///
/// Two things keep the two threads off each other's cache lines:
///  - `head_` (written by the consumer) and `tail_` (written by the producer) live on separate,
///    padded cache lines, so a push doesn't invalidate the line the consumer is spinning on;
///  - each side keeps a private copy of the *other* side's counter and only re-reads the shared
///    atomic when the copy says the queue looks full / empty. In steady state a push touches
///    only producer-owned lines (Rigtorp's SPSCQueue / Vyukov's trick).
/// `Padded = false` turns both off, to measure what they're worth.
template <class T, std::size_t N, bool Padded = true>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_nothrow_copy_assignable_v<T>, "T must be nothrow copy-assignable");
    static constexpr std::size_t kAlign = Padded ? kCacheLine : alignof(std::atomic<std::size_t>);

public:
    static constexpr std::size_t capacity() noexcept { return N; }

    /// Producer only. False if full.
    bool try_push(const T& v) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if constexpr (Padded) {
            if (t - head_cache_ == N) {
                head_cache_ = head_.load(std::memory_order_acquire);
                if (t - head_cache_ == N) return false;
            }
        } else {
            if (t - head_.load(std::memory_order_acquire) == N) return false;
        }
        slots_[t & (N - 1)] = v;
        // release: the slot write above becomes visible before the consumer sees the new tail
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    /// Consumer only. False if empty.
    bool try_pop(T& out) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if constexpr (Padded) {
            if (h == tail_cache_) {
                tail_cache_ = tail_.load(std::memory_order_acquire);
                if (h == tail_cache_) return false;
            }
        } else {
            if (h == tail_.load(std::memory_order_acquire)) return false;
        }
        out = slots_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    /// Approximate, for monitoring only (the other thread may be moving it).
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    alignas(kAlign) std::atomic<std::size_t> head_{0};  // next slot to read (consumer)
    alignas(kAlign) std::size_t tail_cache_ = 0;        // consumer's copy of tail_
    alignas(kAlign) std::atomic<std::size_t> tail_{0};  // next slot to write (producer)
    alignas(kAlign) std::size_t head_cache_ = 0;        // producer's copy of head_
    alignas(kAlign) std::array<T, N> slots_{};
};

}  // namespace matchbook
