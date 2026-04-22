#pragma once
#include <cstdint>
#include "order.h"

// Intrusive doubly-linked list of orders at a single price.
// No heap allocation — Order objects live in the pool; we just wire pointers.
struct PriceLevel {
    Order*   head{nullptr};
    Order*   tail{nullptr};
    uint32_t count{0};
    uint32_t total_quantity{0};

    void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o;
        else      head = o;
        tail = o;
        ++count;
        total_quantity += o->remaining;
    }

    void remove(Order* o) noexcept {
        if (o->prev) o->prev->next = o->next;
        else         head = o->next;
        if (o->next) o->next->prev = o->prev;
        else         tail = o->prev;
        o->next = o->prev = nullptr;
        --count;
        total_quantity -= o->remaining;
    }

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
};
