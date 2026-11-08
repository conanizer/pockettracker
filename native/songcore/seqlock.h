#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// One writer publishes a `T`, one reader copies it out without waiting (a sequence lock). A reader
// that lands on a write keeps its old copy and sees the new one next time.
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
            const uint32_t s = seq_.load(std::memory_order_acquire);
            if (s & 1u) continue;            // a write is in progress: try once more, then keep ours
            if (s == seen) return false;     // nothing new
            if (!copy_at(s, out)) continue;
            seen = s;
            return true;
        }
        return false;
    }

    /** The reader, once: true when `out` now holds the latest value; false if a write was in the way. */
    bool try_read(T& out) const {
        const uint32_t s = seq_.load(std::memory_order_acquire);
        return !(s & 1u) && copy_at(s, out);
    }

    bool published() const { return seq_.load(std::memory_order_acquire) != 0; }

  private:
    // `out` is written only when no write overlapped the copy.
    bool copy_at(uint32_t s, T& out) const {
        uint64_t words[WORDS];
        for (size_t i = 0; i < WORDS; ++i) words[i] = words_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s) return false;
        std::memcpy(&out, words, sizeof out);
        return true;
    }

    static constexpr size_t WORDS = (sizeof(T) + 7) / 8;
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint64_t> words_[WORDS] = {};
};

// N records the control thread edits and the audio thread reads, each in its own copy: `edit` then
// `publish`; the audio thread `sync`s once per block, so a record never changes mid-note.
// ⚠️ `publish` after EVERY edit, or it is never heard.
template <typename T, int N>
class StagedTable {
  public:
    // The control thread.
    T&       edit(int i) { return staged_[i]; }
    const T& staged(int i) const { return staged_[i]; }
    void publish(int i) {
        slots_[i].publish(staged_[i]);
        dirty_[i / 64].fetch_or(uint64_t(1) << (i % 64), std::memory_order_release);
    }

    // The audio thread.
    const T& operator[](int i) const { return live_[i]; }
    void sync() {
        for (int d = 0; d < DIRTY_WORDS; ++d) {
            uint64_t bits = dirty_[d].exchange(0, std::memory_order_acquire);
            for (int b = 0; bits != 0; ++b, bits >>= 1) {
                if (!(bits & 1u)) continue;
                const int i = d * 64 + b;
                if (!slots_[i].try_read(live_[i]))   // a write in the way: try at the next block
                    dirty_[d].fetch_or(uint64_t(1) << b, std::memory_order_relaxed);
            }
        }
    }

  private:
    static constexpr int DIRTY_WORDS = (N + 63) / 64;
    T               staged_[N] = {};
    T               live_[N]   = {};
    SeqPublisher<T> slots_[N];
    std::atomic<uint64_t> dirty_[DIRTY_WORDS] = {};
};
