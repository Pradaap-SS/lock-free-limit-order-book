#pragma once
#include <array>
#include <atomic>
#include <optional>
#include <cstddef>

// Single-Producer Single-Consumer lock-free queue.
// Producer and consumer live on separate cache lines to eliminate false sharing.
// Capacity must be a power of 2.

template<typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr std::size_t MASK = Capacity - 1;

public:
    // Called by producer thread only
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & MASK;
        if (__builtin_expect(next == tail_.load(std::memory_order_acquire), 0))
            return false; // full
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Called by consumer thread only
    std::optional<T> pop() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt; // empty
        T item = buffer_[tail];
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

private:
    alignas(64) std::atomic<std::size_t> head_{0}; // written by producer
    alignas(64) std::atomic<std::size_t> tail_{0}; // written by consumer
    std::array<T, Capacity> buffer_{};
};
