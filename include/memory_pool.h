#pragma once
#include <array>
#include <cstddef>
#include <cassert>
#include <atomic>
#include "order.h"

// Pre-allocated, fixed-size pool of Order objects.
// Zero heap allocations on the critical path — every alloc/free is a pointer
// swap inside a pre-touched memory region.
//
// Single-threaded usage assumed for the matching engine core.
// If multi-threaded access is needed, replace the free-list with a lock-free
// stack (see LockFreeStack below).

template<std::size_t Capacity>
class OrderPool {
public:
    OrderPool() {
        // Build the free list by chaining orders through their 'next' pointer
        for (std::size_t i = 0; i < Capacity - 1; ++i)
            pool_[i].next = &pool_[i + 1];
        pool_[Capacity - 1].next = nullptr;
        free_head_ = &pool_[0];

        // Pre-fault pages so first access doesn't incur page-fault latency
        for (auto& o : pool_) {
            volatile char* p = reinterpret_cast<volatile char*>(&o);
            (void)*p;
        }
    }

    [[nodiscard]] Order* acquire() noexcept {
        if (__builtin_expect(free_head_ == nullptr, 0)) return nullptr;
        Order* o = free_head_;
        free_head_ = o->next;
        o->next = nullptr;
        o->prev = nullptr;
        ++in_use_;
        return o;
    }

    void release(Order* o) noexcept {
        assert(o != nullptr);
        o->next = free_head_;
        o->prev = nullptr;
        free_head_ = o;
        --in_use_;
    }

    [[nodiscard]] std::size_t available() const noexcept { return Capacity - in_use_; }
    [[nodiscard]] std::size_t in_use()    const noexcept { return in_use_; }

private:
    alignas(64) std::array<Order, Capacity> pool_;
    Order*      free_head_{nullptr};
    std::size_t in_use_{0};
};
