#pragma once

#include <cstddef>
#include <vector>

namespace matchbook {

/// Fixed-capacity object pool. All storage is allocated once in the constructor, so acquire and
/// release never touch the heap: on the matching path a `new` is a potential page fault or lock
/// inside the allocator, both far slower than the work we actually want to do.
template <class T>
class Pool {
public:
    explicit Pool(std::size_t capacity) : slots_(capacity) {
        free_.reserve(capacity);
        // hand out low addresses first, slightly friendlier to the cache when the pool is big
        for (std::size_t i = capacity; i-- > 0;) free_.push_back(&slots_[i]);
    }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    /// nullptr when the pool is exhausted; the caller decides how to reject the request.
    [[nodiscard]] T* acquire() noexcept {
        if (free_.empty()) return nullptr;
        T* p = free_.back();
        free_.pop_back();
        *p = T{};  // never leak state from the previous user of the slot
        return p;
    }

    void release(T* p) noexcept { free_.push_back(p); }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t available() const noexcept { return free_.size(); }
    [[nodiscard]] std::size_t in_use() const noexcept { return slots_.size() - free_.size(); }

private:
    std::vector<T> slots_;  // never resized after construction, so pointers stay valid
    std::vector<T*> free_;
};

}  // namespace matchbook
