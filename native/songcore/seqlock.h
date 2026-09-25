#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

/**
 * One writer publishes a `T`, one reader copies it out without ever waiting.
 *
 * A sequence lock: the writer bumps `seq_` to odd, writes, bumps to even; the reader copies and
 * keeps the copy only if `seq_` was even and unchanged across it. The words are atomics so the
 * overlap is defined; a reader that lands on a write in progress keeps the copy it already had and
 * sees the new one next time.
 */
template <typename T>
class SeqPublisher {
    static_assert(std::is_trivially_copyable<T>::value, "published as raw words");

  public:
    /** The writer. */
    void publish(const T& v) {
        uint64_t words[WORDS] = {0};
        std::memcpy(words, &v, sizeof v);
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < WORDS; ++i) words_[i].store(words[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }

    /** The reader. True when `out` now holds a value newer than `seen` (which is updated). */
    bool read(T& out, uint32_t& seen) const {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) continue;            // a write is in progress: try once more, then keep ours
            if (s1 == seen) return false;     // nothing new
            uint64_t words[WORDS];
            for (size_t i = 0; i < WORDS; ++i) words[i] = words_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) != s1) continue;
            std::memcpy(&out, words, sizeof out);
            seen = s1;
            return true;
        }
        return false;
    }

    bool published() const { return seq_.load(std::memory_order_acquire) != 0; }

  private:
    static constexpr size_t WORDS = (sizeof(T) + 7) / 8;
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint64_t> words_[WORDS] = {};
};

/**
 * N records the control thread edits and the audio thread reads, neither waiting on the other.
 *
 * Each side has its own copy. The control thread changes an entry with `edit` and then `publish`es
 * it; the audio thread pulls every published entry into ITS copy at one point in its block (`sync`)
 * and reads only that copy (`[]`), so a record never changes under it halfway through a note. An
 * entry the pull finds mid-write stays marked and is taken on the next block.
 *
 * ⚠️ `publish` after EVERY edit. An edit that is not published is simply never heard.
 */
template <typename T, int N>
class StagedTable {
    static_assert(std::is_trivially_copyable<T>::value, "published as raw words");

  public:
    // The control thread.
    T&       edit(int i) { return staged_[i]; }
    const T& staged(int i) const { return staged_[i]; }
    void publish(int i) {
        uint64_t words[WORDS] = {0};
        std::memcpy(words, &staged_[i], sizeof(T));
        Slot& s = slots_[i];
        const uint32_t q = s.seq.load(std::memory_order_relaxed);
        s.seq.store(q + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t w = 0; w < WORDS; ++w) s.words[w].store(words[w], std::memory_order_relaxed);
        s.seq.store(q + 2, std::memory_order_release);
        dirty_[i / 64].fetch_or(uint64_t(1) << (i % 64), std::memory_order_release);
    }

    // The audio thread.
    const T& operator[](int i) const { return live_[i]; }
    void sync() {
        for (int d = 0; d < DIRTY_WORDS; ++d) {
            uint64_t bits = dirty_[d].exchange(0, std::memory_order_acquire);
            for (int b = 0; bits != 0; ++b, bits >>= 1) {
                if (!(bits & 1u)) continue;
                if (!pull(d * 64 + b)) dirty_[d].fetch_or(uint64_t(1) << b, std::memory_order_relaxed);
            }
        }
    }

  private:
    bool pull(int i) {
        const Slot& s = slots_[i];
        const uint32_t q = s.seq.load(std::memory_order_acquire);
        if (q & 1u) return false;
        uint64_t words[WORDS];
        for (size_t w = 0; w < WORDS; ++w) words[w] = s.words[w].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.seq.load(std::memory_order_relaxed) != q) return false;
        std::memcpy(&live_[i], words, sizeof(T));
        return true;
    }

    static constexpr size_t WORDS       = (sizeof(T) + 7) / 8;
    static constexpr int    DIRTY_WORDS = (N + 63) / 64;
    struct Slot {
        std::atomic<uint32_t> seq{0};
        std::atomic<uint64_t> words[WORDS] = {};
    };
    T    staged_[N] = {};
    T    live_[N]   = {};
    Slot slots_[N];
    std::atomic<uint64_t> dirty_[DIRTY_WORDS] = {};
};
