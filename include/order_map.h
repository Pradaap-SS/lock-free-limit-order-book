#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include "order.h"

// Open-addressing hash map: OrderId → Order*.
// Power-of-2 capacity, linear probing, tombstone-free deletion via
// backward-shift. No STL, no heap allocations.

template<std::size_t Capacity>
class OrderMap {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr std::size_t MASK = Capacity - 1;
    static constexpr OrderId     EMPTY = 0;  // 0 is never a valid order ID

    struct Slot {
        OrderId key{EMPTY};
        Order*  val{nullptr};
    };

public:
    OrderMap() { slots_.fill({}); }

    bool insert(OrderId id, Order* o) noexcept {
        std::size_t i = hash(id);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            Slot& s = slots_[(i + probe) & MASK];
            if (s.key == EMPTY) {
                s.key = id;
                s.val = o;
                ++size_;
                return true;
            }
        }
        return false; // full
    }

    [[nodiscard]] Order* find(OrderId id) const noexcept {
        std::size_t i = hash(id);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            const Slot& s = slots_[(i + probe) & MASK];
            if (s.key == id)    return s.val;
            if (s.key == EMPTY) return nullptr;
        }
        return nullptr;
    }

    bool erase(OrderId id) noexcept {
        std::size_t i = hash(id);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            std::size_t pos = (i + probe) & MASK;
            if (slots_[pos].key == id) {
                backward_shift(pos);
                --size_;
                return true;
            }
            if (slots_[pos].key == EMPTY) return false;
        }
        return false;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    static std::size_t hash(OrderId id) noexcept {
        // Fibonacci hashing for good dispersion
        return static_cast<std::size_t>(id * 11400714819323198485ULL) & MASK;
    }

    void backward_shift(std::size_t pos) noexcept {
        // Shift subsequent slots left to patch the gap (no tombstones needed)
        for (;;) {
            std::size_t next = (pos + 1) & MASK;
            if (slots_[next].key == EMPTY) {
                slots_[pos] = {};
                return;
            }
            std::size_t natural = hash(slots_[next].key);
            // Only shift if the natural slot is at or before pos in probe order
            bool should_shift = ((next - natural) & MASK) > ((next - pos - 1) & MASK);
            if (!should_shift) {
                slots_[pos] = {};
                return;
            }
            slots_[pos] = slots_[next];
            pos = next;
        }
    }

    alignas(64) std::array<Slot, Capacity> slots_;
    std::size_t size_{0};
};
