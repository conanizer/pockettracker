#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

// One row of a table — a 16-row sequencer that runs beside a playing voice.
struct TableRow {
    struct Fx { uint8_t type = 0, value = 0; };   // type 0 = none
    int8_t  transpose = 0;      // semitones, signed
    uint8_t volume    = 0xFF;   // FF = no change
    Fx      fx[3];              // FX1..FX3 = lanes 0..2
};

static_assert(sizeof(TableRow) == 8, "a table row is one 64-bit word in TableStore");

// The engine's 256 tables: one writer (the UI's `loadTable`), audio-thread readers that never wait.
// The writer fills the copy readers are not pointed at, then swaps; a per-copy sequence number
// catches two writes inside one read, and the reader reads again.
class TableStore {
  public:
    static constexpr int TABLES = 256;
    static constexpr int ROWS   = 16;

    // The caller serialises writers.
    void write(int id, const TableRow (&rows)[ROWS]) {
        Entry& e = entries_[id];
        const int next = e.current.load(std::memory_order_relaxed) ^ 1;
        Copy& c = e.copies[next];
        const uint32_t s = c.seq.load(std::memory_order_relaxed);
        c.seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int r = 0; r < ROWS; ++r) {
            uint64_t w;
            std::memcpy(&w, &rows[r], sizeof w);
            c.rows[r].store(w, std::memory_order_relaxed);
        }
        c.seq.store(s + 2, std::memory_order_release);
        e.current.store(next, std::memory_order_release);
        e.loaded.store(true, std::memory_order_release);
    }

    // Any thread, never waits. False when the table was never loaded.
    bool read(int id, TableRow (&out)[ROWS]) const {
        const Entry& e = entries_[id];
        if (!e.loaded.load(std::memory_order_acquire)) return false;
        // Bounded: eight retries cannot happen at editing speed; if they did, skip rather than tear.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const Copy& c = e.copies[e.current.load(std::memory_order_acquire)];
            const uint32_t s1 = c.seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            uint64_t words[ROWS];
            for (int r = 0; r < ROWS; ++r) words[r] = c.rows[r].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (c.seq.load(std::memory_order_relaxed) != s1) continue;
            std::memcpy(out, words, sizeof words);
            return true;
        }
        return false;
    }

  private:
    struct Copy {
        std::atomic<uint32_t> seq{0};
        std::atomic<uint64_t> rows[ROWS] = {};
    };
    struct Entry {
        Copy copies[2];
        std::atomic<int>  current{0};
        std::atomic<bool> loaded{false};
    };
    Entry entries_[TABLES];
};

inline int transposeToSemitones(uint8_t transpose) {
    if (transpose < 0x80) {
        return transpose;  // 00-7F = 0 to +127
    } else {
        return transpose - 256;  // 80-FF = -128 to -1
    }
}
