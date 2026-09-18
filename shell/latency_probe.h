// latency_probe.h — what the app can know about its own latency, measured rather than read.
//
// latency-review.md §1 puts two large terms in the budget and both are READINGS, not results: the wait
// for the app to look at an input, and the time an audio block spends in the output buffer (claimed
// 12–42 ms). This measures the first outright and the honest half of the second, and it is
// deliberately loud about the half it cannot see. `ptlat` and a microphone are the only things that
// can see that half.
//
// ⚠️ **TWO PERIODS, BECAUSE THE LOOP HAS TWO RATES** (app.cpp, THE TWO RATES). The POLL period is the
// one that costs an input its wait; the FRAME period is the drawing, and it is here because the split
// that made the first small is exactly the thing that could quietly wreck the second.
//
// ⚠️ **A GLOBAL, AND DELIBERATELY NOT ON THE AUDIO SEAM.** The frame loop and the audio callback are
// on different threads in different files, and the only object they share is `AudioBackend`. That seam
// reports what the DEVICE is doing — its rate, its latency — and carries no instrument: accumulators
// put there would be shipped on every platform forever for the sake of a diagnostic that is off by
// default. So they sit here, written with relaxed atomics from both sides and read once on the way out,
// and `report` is handed the seam's latency figure as an argument.
//
// ⚠️ **TWO WINDOWS, NOT ONE, AND THE SPLIT IS THE POINT.** §11: a figure taken with the transport
// stopped and one taken under a full mix are different claims. A single session average silently
// blends them in whatever ratio the operator happened to play, so every number below is binned by
// whether the transport was running.
//
// ⚠️ **THE CALLBACK GAP IS SDL-ONLY.** `SdlAudioEngine::audioCallback` calls it; Oboe's callback does
// not, because `oboe-audio-engine.cpp` is below `shell/` and cannot see this header. Android's gap rows
// are therefore empty — its output figure comes through the seam instead, which is the one term of the
// two that Oboe can report better than this header could measure it.
//
//   POCKETTRACKER_LATENCY=1   accumulate, and print the account on exit
//
// Off by default and free when off: one cached bool ahead of every store.

#ifndef POCKETTRACKER_LATENCY_PROBE_H
#define POCKETTRACKER_LATENCY_PROBE_H

#include <SDL.h>

#include "audio-backend.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace latency {

/**
 * One binned accumulator. Min/mean/max of an interval, plus the two counts that say something the
 * mean cannot: how often it ran long, and how often it did not wait at all.
 *
 * ⚠️ `over` and `burst` are the interesting ones. A loop whose MEAN is 16.7 ms and whose max is 40 is
 * not a 16.7 ms loop for the press that landed in the 40. And a run of callbacks arriving back to back
 * is a driver handing over several buffers at once — the only hint from inside the process about the
 * queue depth §1 admits it does not know.
 */
struct Bin {
    std::atomic<uint64_t> n{0}, sumNs{0}, maxNs{0}, over{0}, burst{0};
    std::atomic<uint64_t> minNs{~uint64_t(0)};

    void add(uint64_t ns, uint64_t overNs, uint64_t burstNs) {
        n.fetch_add(1, std::memory_order_relaxed);
        sumNs.fetch_add(ns, std::memory_order_relaxed);
        if (ns > maxNs.load(std::memory_order_relaxed)) maxNs.store(ns, std::memory_order_relaxed);
        if (ns < minNs.load(std::memory_order_relaxed)) minNs.store(ns, std::memory_order_relaxed);
        if (ns > overNs) over.fetch_add(1, std::memory_order_relaxed);
        if (ns < burstNs) burst.fetch_add(1, std::memory_order_relaxed);
    }

    double meanMs() const {
        const uint64_t c = n.load(std::memory_order_relaxed);
        return c ? double(sumNs.load(std::memory_order_relaxed)) / double(c) / 1e6 : 0.0;
    }
    double minMs() const {
        const uint64_t v = minNs.load(std::memory_order_relaxed);
        return v == ~uint64_t(0) ? 0.0 : double(v) / 1e6;
    }
    double maxMs() const { return double(maxNs.load(std::memory_order_relaxed)) / 1e6; }
};

struct State {
    Bin poll[2];        // [0] transport stopped, [1] playing
    Bin frame[2];
    Bin cb[2];

    std::atomic<int>  playing{0};
    std::atomic<int>  frames{0};        // per callback, as the DEVICE hands it over
    std::atomic<bool> sawCallback{false};

    uint64_t lastPollNs  = 0;                     // the app loop's own — single-threaded, no atomic
    uint64_t lastFrameNs = 0;
    std::atomic<uint64_t> lastCbNs{0};
};

inline State& state() {
    static State s;
    return s;
}

inline bool enabled() {
    static const bool on = [] {
        const char* e = SDL_getenv("POCKETTRACKER_LATENCY");
        return e && e[0] == '1';
    }();
    return on;
}

inline uint64_t now_ns() {
    // Integer arithmetic split around the division, for the reason sdl-audio-engine.cpp gives: the
    // counter times 1e9 overflows uint64 on a box that has been up a few weeks.
    static const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64        c    = SDL_GetPerformanceCounter();
    return (c / freq) * 1000000000ull + ((c % freq) * 1000000000ull) / freq;
}

/**
 * Top of the app loop, every tick. `isPlaying` bins this sample AND tells the audio thread which bin
 * it is in.
 *
 * "Long" is twice the nominal 4 ms tick; "tight" is under a millisecond, which is a tick that did not
 * wait at all — one following a present that blocked past its deadline.
 */
inline void poll_tick(bool isPlaying) {
    if (!enabled()) return;
    State&         s  = state();
    const uint64_t t  = now_ns();
    s.playing.store(isPlaying ? 1 : 0, std::memory_order_relaxed);
    if (s.lastPollNs != 0) s.poll[isPlaying ? 1 : 0].add(t - s.lastPollNs, 8000000ull, 1000000ull);
    s.lastPollNs = t;
}

/**
 * The ticks that also DRAW, so the split can be read in one place: the poll row should fall to ~4 ms
 * while this one stays at the display's period.
 *
 * ⚠️ "Long" is a dropped frame (twice 16.7 ms) and "tight" is two frames inside one refresh — which
 * the frame deadline exists to prevent, so a non-zero count there is the deadline not holding.
 */
inline void frame_tick(bool isPlaying) {
    if (!enabled()) return;
    State&         s = state();
    const uint64_t t = now_ns();
    if (s.lastFrameNs != 0) s.frame[isPlaying ? 1 : 0].add(t - s.lastFrameNs, 33000000ull, 8000000ull);
    s.lastFrameNs = t;
}

/**
 * The audio callback, from the audio thread.
 *
 * ⚠️ Relaxed atomics and nothing else — no lock, no allocation, no printf. This runs in the same place
 * the engine's DSP does, and an instrument that perturbs the thing it measures is worse than none.
 *
 * `frames` is what the device actually handed over, not what was asked for. That distinction is the
 * whole of §11's warning about a boot line printing 512 while the device runs 940.
 */
inline void audio_callback(int frames) {
    if (!enabled()) return;
    State&         s = state();
    const uint64_t t = now_ns();
    s.frames.store(frames, std::memory_order_relaxed);
    s.sawCallback.store(true, std::memory_order_relaxed);
    const uint64_t last = s.lastCbNs.exchange(t, std::memory_order_relaxed);
    if (last != 0) {
        const int bin = s.playing.load(std::memory_order_relaxed);
        // "Ran long" is twice the nominal block; "back to back" is a tenth of it. Both are computed
        // from `frames`, so they follow whatever the device negotiated rather than a constant.
        const uint64_t nominal = uint64_t(frames) * 1000000000ull / 44100ull;
        s.cb[bin].add(t - last, nominal * 2, nominal / 10);
    }
}

inline void print_bin(const char* what, const Bin& b, const char* unit) {
    const uint64_t c = b.n.load(std::memory_order_relaxed);
    if (c == 0) { std::printf("  %-22s nothing recorded\n", what); return; }
    std::printf("  %-22s n %-7llu  mean %7.2f  min %7.2f  max %7.2f ms   long %llu   %s %llu\n", what,
                (unsigned long long)c, b.meanMs(), b.minMs(), b.maxMs(),
                (unsigned long long)b.over.load(std::memory_order_relaxed), unit,
                (unsigned long long)b.burst.load(std::memory_order_relaxed));
}

/**
 * The account, on the way out.
 *
 * ⚠️ **IT NAMES WHAT IT DID NOT MEASURE INSTEAD OF QUIETLY LEAVING IT OUT OF THE SUM.** When the
 * backend hands over a floor rather than a figure — always on SDL, and on Oboe wherever the platform
 * withholds a timestamp — the driver's own queue is missing from the total, and a confident number
 * there would be worse than no number. The sum says "at least" and points at `ptlat` in that case.
 *
 * ⚠️ Refuses to judge a bin below 200 samples and says so — a mean period from a handful of frames is
 * noise, and a session where the transport never ran would otherwise print a playing row of zeros that
 * reads like a result.
 */
inline void report(int sampleRate, int leadFrames, AudioBackend::OutputLatency out) {
    if (!enabled()) return;
    State&     s  = state();
    const int  fr = s.frames.load(std::memory_order_relaxed);
    const int  sr = sampleRate > 0 ? sampleRate : 44100;

    std::printf("\nlatency: the app's own account, measured this run (POCKETTRACKER_LATENCY=1)\n");
    for (int bin = 0; bin < 2; ++bin) {
        const char* when = bin ? "playing" : "stopped";
        std::printf("\n  -- transport %s --\n", when);
        print_bin("poll period", s.poll[bin], "tight");
        print_bin("drawn frame period", s.frame[bin], "double");
        print_bin("audio callback gap", s.cb[bin], "back-to-back");
        const uint64_t c = s.poll[bin].n.load(std::memory_order_relaxed);
        if (c < 200) std::printf("  ⚠ only %llu poll samples — too few to mean anything\n",
                                 (unsigned long long)c);
    }

    if (!s.sawCallback.load(std::memory_order_relaxed)) {
        std::printf("\n  ⚠ the audio callback never reported. On Android it does not — Oboe's callback is\n"
                    "    below shell/ and cannot reach this header, though the output row below still\n"
                    "    comes from the device. Elsewhere it means no device at all.\n");
    }

    // ⚠️ The buffer is read back TWICE by two different paths — the seam asks the device, the callback
    // counts what it was handed — and they are printed against each other rather than picked between.
    // On SDL they are the same negotiated number; a disagreement means one of them is not reading the
    // device, and that is exactly the lie this instrument exists to catch.
    if (fr > 0 && out.frames > 0 && fr != out.frames) {
        std::printf("\n  ⚠ the device disagrees with itself: the seam reports %d frames, the callback was\n"
                    "    handed %d. One of the two is not reading the device.\n", out.frames, fr);
    }

    // The sum, and the missing term named beside it. The poll row is HALVED because a gesture arrives
    // uniformly inside the period, so the average wait is half of it — the worst case is the whole.
    const Bin&   busiest = s.poll[1].n.load(std::memory_order_relaxed) >= s.poll[0].n.load(std::memory_order_relaxed)
                               ? s.poll[1] : s.poll[0];
    const double loopMean = busiest.meanMs();
    const double loopMax  = busiest.maxMs();
    const double leadMs   = 1000.0 * double(leadFrames) / double(sr);
    const double outMs    = out.frames > 0 ? 1000.0 * double(out.frames) / double(sr) : 0.0;

    std::printf("\n  gesture -> sound, as far as this process can see it\n");
    std::printf("    waiting for the poll to look    %5.2f mean, %5.2f worst ms\n", loopMean / 2.0, loopMax);
    std::printf("    the note's lead-in              %5.2f ms   (%d frames at %d Hz)\n", leadMs, leadFrames, sr);
    std::printf("    the output path                 %5.2f ms   (%d frames, %s)\n", outMs, out.frames,
                out.measured ? "the platform's own figure, its queue included"
                             : "the app's buffer alone, read back from the device");
    std::printf("    ------------------------------------------------------------------\n");
    if (out.measured) {
        std::printf("    total                           %5.2f ms — every term accounted for, and the\n",
                    loopMean / 2.0 + leadMs + outMs);
        std::printf("    last one is still the platform's claim about itself. One ptlat recording settles it.\n");
    } else {
        std::printf("    at least                        %5.2f ms, and the driver's own queue is NOT in\n",
                    loopMean / 2.0 + leadMs + outMs);
        std::printf("    that sum — it is the term ptlat exists for. Nothing in this process can hear\n"
                    "    its own speaker.\n");
    }
    std::fflush(stdout);
}

}  // namespace latency

#endif  // POCKETTRACKER_LATENCY_PROBE_H
