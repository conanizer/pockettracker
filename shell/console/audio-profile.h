#ifndef POCKETTRACKER_SHELL_AUDIO_PROFILE_H
#define POCKETTRACKER_SHELL_AUDIO_PROFILE_H

// POCKETTRACKER_AUDIO_PROFILE (any value): each SDL audio block's time against its budget and the gap
// between callbacks, printed once a second by the frame loop. Block over budget = a compute underrun;
// a big gap with a fast block = the thread was preempted.
// ⚠️ The callback's half is relaxed atomics only — no lock, allocation or printf where the DSP runs.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "console/latency-probe.h"   // now_ns

namespace audio_profile {

struct State {
    std::atomic<uint64_t> count{0}, over{0}, sumBlockNs{0}, maxBlockNs{0}, maxGapNs{0}, budgetNs{0};
    std::atomic<int>      frames{0};
    uint64_t              lastStartNs = 0;   // the audio thread's only
    uint64_t              lastPrintNs = 0;   // the frame loop's only
};

inline State& state() {
    static State s;
    return s;
}

inline bool enabled() {
    static const bool on = std::getenv("POCKETTRACKER_AUDIO_PROFILE") != nullptr;
    return on;
}

inline void raise_to(std::atomic<uint64_t>& a, uint64_t v) {
    if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}

// From the audio callback: one block that started at `startNs` and ended at `endNs`.
inline void block(uint64_t startNs, uint64_t endNs, int frames, int sampleRate) {
    State& s = state();
    if (s.lastStartNs != 0) raise_to(s.maxGapNs, startNs - s.lastStartNs);
    s.lastStartNs = startNs;

    const uint64_t took   = endNs - startNs;
    const uint64_t budget = uint64_t(frames) * 1000000000ull / uint64_t(sampleRate > 0 ? sampleRate : 44100);
    raise_to(s.maxBlockNs, took);
    s.sumBlockNs.fetch_add(took, std::memory_order_relaxed);
    s.count.fetch_add(1, std::memory_order_relaxed);
    if (took > budget) s.over.fetch_add(1, std::memory_order_relaxed);
    s.budgetNs.store(budget, std::memory_order_relaxed);
    s.frames.store(frames, std::memory_order_relaxed);
}

// From the frame loop, every tick: the last second's line, then a fresh second.
inline void print_due() {
    if (!enabled()) return;
    State&         s   = state();
    const uint64_t now = latency::now_ns();
    if (s.lastPrintNs == 0) s.lastPrintNs = now;
    if (now - s.lastPrintNs < 1000000000ull) return;
    s.lastPrintNs = now;

    const uint64_t n   = s.count.exchange(0, std::memory_order_relaxed);
    const uint64_t sum = s.sumBlockNs.exchange(0, std::memory_order_relaxed);
    std::printf("PROF: n=%llu avgBlk=%.2fms maxBlk=%.2fms over=%llu maxGap=%.2fms budget=%.2fms frames=%d\n",
                (unsigned long long)n, n ? double(sum) / double(n) / 1e6 : 0.0,
                double(s.maxBlockNs.exchange(0, std::memory_order_relaxed)) / 1e6,
                (unsigned long long)s.over.exchange(0, std::memory_order_relaxed),
                double(s.maxGapNs.exchange(0, std::memory_order_relaxed)) / 1e6,
                double(s.budgetNs.load(std::memory_order_relaxed)) / 1e6,
                s.frames.load(std::memory_order_relaxed));
    std::fflush(stdout);
}

}  // namespace audio_profile

#endif  // POCKETTRACKER_SHELL_AUDIO_PROFILE_H
