#pragma once
#include <atomic>
#include <cstdint>

namespace songcore {

// A ring between two threads: one producer, one consumer, no lock; each side owns one index. When it
// is full the NEWEST items are dropped and counted, never silently.
template <typename T, int N>
class SpscRing {
    static_assert(N > 0 && (N & (N - 1)) == 0, "a power of two: the slot is `index & MASK`");

  public:
    static constexpr int CAPACITY = N;

    // ── the producer ──

    // Writes `make(i)` for i = 0…n-1 while there is room, under one publish; returns how many went in.
    template <typename Make>
    int push_each(int n, Make&& make) {
        const uint32_t head = head_.load(std::memory_order_acquire);
        uint32_t       tail = tail_.load(std::memory_order_relaxed);
        int i = 0;
        for (; i < n && tail - head < static_cast<uint32_t>(N); ++i, ++tail) items_[tail & MASK] = make(i);
        if (i < n) dropped_.fetch_add(static_cast<uint64_t>(n - i), std::memory_order_relaxed);
        tail_.store(tail, std::memory_order_release);
        return i;
    }
    bool push(const T& v) { return push_each(1, [&](int) { return v; }) == 1; }

    // ── the consumer ──

    // Hands up to `max` items, oldest first, to `take(i, item)`; returns how many.
    template <typename Take>
    int pop_each(int max, Take&& take) {
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        uint32_t       head = head_.load(std::memory_order_relaxed);
        int n = 0;
        for (; head != tail && n < max; ++head) take(n++, items_[head & MASK]);
        head_.store(head, std::memory_order_release);
        return n;
    }
    bool pop(T& out) { return pop_each(1, [&](int, const T& v) { out = v; }) == 1; }

    void clear() { head_.store(tail_.load(std::memory_order_acquire), std::memory_order_release); }

    // Drops everything written before `mark` (from `written`).
    void discard_up_to(uint32_t mark) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        if (static_cast<int32_t>(mark - head) > 0) head_.store(mark, std::memory_order_release);
    }

    // ── any thread ──

    uint32_t written() const { return tail_.load(std::memory_order_acquire); }
    int pending() const {
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<int>(tail - head_.load(std::memory_order_acquire));
    }
    // Items lost to a full ring, ever.
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  private:
    static constexpr uint32_t MASK = N - 1;

    T                     items_[N] = {};
    std::atomic<uint32_t> head_{0};   // the consumer's
    std::atomic<uint32_t> tail_{0};   // the producer's
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace songcore
