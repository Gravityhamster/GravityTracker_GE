#pragma once
#include <atomic>
#include <cstddef>

// Claude -- Queue implementation for single producer single consumer (SPSC) queue
template <typename T, size_t N>
class SpscQueue
{
    // Size of the queue must be a power of two for the bitwise AND to work correctly
    static_assert((N& (N - 1)) == 0, "N must be a power of two");
    T buf[N];
    std::atomic<size_t> head{ 0 }; // advanced by the consumer (audio thread)
    std::atomic<size_t> tail{ 0 }; // advanced by the producer (tracker thread)

public:
    bool push(const T& v) // producer only
    {
        size_t t = tail.load(std::memory_order_relaxed);
        if (t - head.load(std::memory_order_acquire) == N)
            return false; // full
        buf[t & (N - 1)] = v;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) // consumer only
    {
        size_t h = head.load(std::memory_order_relaxed);
        if (h == tail.load(std::memory_order_acquire))
            return false; // empty
        out = buf[h & (N - 1)];
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pop() // consumer only (no out)
    {
        size_t h = head.load(std::memory_order_relaxed);
        if (h == tail.load(std::memory_order_acquire))
            return false; // empty
        head.store(h + 1, std::memory_order_release);
        return true;
    }
};