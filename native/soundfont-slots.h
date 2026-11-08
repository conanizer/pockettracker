#pragma once
#include <atomic>
#include <cstdint>
#include <string>

struct tsf;   // defined in soundfont-voice.cpp

// One slot per loaded PRESET: eight tracks on eight sounds of one file take eight, plus the preview
// lane and the file being browsed. ⚠️ `g_sfBig` in soundfont-voice.cpp sizes off this.
static const int MAX_SOUNDFONTS = 12;

// ⚠️ The audio thread reads `handle` and `gen` only, without a lock: `freeSoundfontSlot` swaps the
// handle out and waits for the block boundary before closing it. A voice whose slot's `gen` moved
// since it was armed is detached at the next block.
struct SoundfontEntry {
    std::atomic<tsf*>     handle{nullptr};
    std::atomic<uint32_t> gen{0};  // bumped by every free
    int instrumentId = -1;         // owner; -1 = free
    // ⚠️ A slot is identified by path AND bank AND preset — one file at two sounds is two slots.
    std::string filePath;
    int bank = -1, preset = -1;
    std::atomic<uint64_t> lastUsed{0};  // LRU tick; 0 = never used
};

extern SoundfontEntry soundfonts[MAX_SOUNDFONTS];

// The LRU clock: bumped on load and on every note, so the slot playing now is never the one evicted.
inline uint64_t nextSfUseTick() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}
