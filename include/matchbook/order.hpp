#pragma once

#include <cstdint>

#include "matchbook/types.hpp"

namespace matchbook {

struct Level;

/// A resting order. The prev/next links make it a node of its price level's FIFO queue
/// (intrusive list), so removing it from the middle of the queue is O(1) with no search.
struct Order {
    OrderId id = 0;
    Price price = 0;
    Qty qty = 0;  // remaining quantity
    Side side = Side::Buy;
    Order* prev = nullptr;
    Order* next = nullptr;
    Level* level = nullptr;
};

/// All resting orders at one price, oldest first (time priority).
struct Level {
    Price price = 0;
    Qty total_qty = 0;
    std::uint32_t count = 0;
    Order* head = nullptr;
    Order* tail = nullptr;

    void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail) {
            tail->next = o;
        } else {
            head = o;
        }
        tail = o;
        o->level = this;
        total_qty += o->qty;
        ++count;
    }

    void erase(Order* o) noexcept {
        // relink the neighbours; works for head, tail and middle alike
        (o->prev ? o->prev->next : head) = o->next;
        (o->next ? o->next->prev : tail) = o->prev;
        total_qty -= o->qty;
        --count;
        o->prev = o->next = nullptr;
        o->level = nullptr;
    }

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
};

}  // namespace matchbook
