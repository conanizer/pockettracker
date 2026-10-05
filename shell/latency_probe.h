// latency_probe.h — what the app can know about its own latency, measured rather than read.
//
// Two large terms in the latency budget are usually READINGS, not results: the wait for the app to
// look at an input, and the time an audio block spends in the output buffer. This measures the first
// and the visible half of the second, and is loud about the half it cannot see (only a microphone can).
//
// ⚠️ TWO PERIODS, BECAUSE THE LOOP HAS TWO RATES (app.cpp): the POLL period costs an input its wait;
// the FRAME period is the drawing, kept here because splitting the two could quietly wreck the second.
//
// ⚠️ A GLOBAL, NOT ON THE AUDIO SEAM: `AudioBackend` reports what the device does and carries no
// instrument, so a default-off diagnostic does not ship in it. Written with relaxed atomics from both
// threads, read once on the way out; `report` is handed the seam's latency figure.
//
// ⚠️ TWO WINDOWS: a figure taken with the transport stopped and one under a full mix are different
// claims, so every number below is binned by whether the transport was running.
//
// ⚠️ THE CALLBACK GAP IS SDL-ONLY: oboe-audio-engine.cpp is below shell/ and cannot see this header.
// Android's output figure comes through the seam instead.
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
 * ⚠️ `over` and `burst` are the interesting ones: a 16.7 ms mean with a 40 ms max is not a 16.7 ms
 * loop for the press that landed in the 40, and callbacks back to back are a driver handing over
 * several buffers at once — the only hint from inside the process about its queue depth.
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
    if (s.lastPollNs != 0) {
        const uint64_t gap = t - s.lastPollNs;
        s.poll[isPlaying ? 1 : 0].add(gap, 8000000ull, 1000000ull);
        // Each one as it happens, timestamped, so a stall can be matched to what was being done: the
        // sequencer is not refilled while the loop is away, so the lookahead must outlast the longest.
        // ⚠️ Above the 33 ms a load refills at (`LOADING_REPAINT_MS`), or every load would print.
        if (isPlaying && gap > 50000000ull) {
            std::printf("latency: STALL %.1f ms while playing, at %.2f s\n", double(gap) / 1e6,
                        double(SDL_GetTicks64()) / 1000.0);
            std::fflush(stdout);
        }
    }
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
 * `frames` is what the device actually handed over, not what was asked for (a boot line can print
 * 512 while the device runs 940).
 */
inline void audio_callback(int frames, int sampleRate) {
    if (!enabled()) return;
    State&         s = state();
    const uint64_t t = now_ns();
    s.frames.store(frames, std::memory_order_relaxed);
    s.sawCallback.store(true, std::memory_order_relaxed);
    const uint64_t last = s.lastCbNs.exchange(t, std::memory_order_relaxed);
    if (last != 0) {
        const int bin = s.playing.load(std::memory_order_relaxed);
        // "Ran long" is twice the nominal block, "back to back" a tenth of it — both from the
        // negotiated frames AND rate (a constant 44100 is 8.8% loose on a 48 kHz device).
        const uint64_t sr      = uint64_t(sampleRate > 0 ? sampleRate : 44100);
        const uint64_t nominal = uint64_t(frames) * 1000000000ull / sr;
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
 * ⚠️ IT NAMES WHAT IT DID NOT MEASURE: when the backend hands over a floor (always on SDL), the
 * driver's queue is missing from the total, and the sum says "at least".
 *
 * ⚠️ Refuses to judge a bin below 200 samples — a session where the transport never ran would
 * otherwise print a playing row of zeros that reads like a result.
 */
inline void report(int sampleRate, bool inputPolled, AudioBackend::OutputLatency out) {
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

    // The sum, the missing term named beside it. A waiting row is HALVED: a gesture arrives uniformly
    // inside the period. ⚠️ The poll is in a MIDI key's path only for a pumped backend (Android); a
    // pushed byte waits only for the audio thread's next block.
    const Bin&   busiest = s.poll[1].n.load(std::memory_order_relaxed) >= s.poll[0].n.load(std::memory_order_relaxed)
                               ? s.poll[1] : s.poll[0];
    const Bin&   cbBusy  = s.cb[1].n.load(std::memory_order_relaxed) >= s.cb[0].n.load(std::memory_order_relaxed)
                               ? s.cb[1] : s.cb[0];
    const double loopMean  = busiest.meanMs();
    const double loopMax   = busiest.maxMs();
    const bool   cbSeen    = cbBusy.n.load(std::memory_order_relaxed) > 0;
    const double blockMean = cbSeen ? cbBusy.meanMs() : (fr > 0 ? 1000.0 * double(fr) / double(sr) : 0.0);
    const double blockMax  = cbSeen ? cbBusy.maxMs()  : blockMean;
    const double outMs     = out.frames > 0 ? 1000.0 * double(out.frames) / double(sr) : 0.0;
    const double pollMs    = inputPolled ? loopMean / 2.0 : 0.0;

    std::printf("\n  gesture -> sound, as far as this process can see it\n");
    if (inputPolled)
        std::printf("    waiting for the poll to pump    %5.2f mean, %5.2f worst ms   (a polled MIDI port)\n",
                    loopMean / 2.0, loopMax);
    else
        std::printf("    the poll                        not in the path: the port pushes, the audio thread drains\n");
    std::printf("    waiting for the next audio block %5.2f mean, %5.2f worst ms  (%s)\n", blockMean / 2.0, blockMax,
                cbSeen ? "the callback's own period" : "nominal — the callback never reported");
    std::printf("    the output path                 %5.2f ms   (%d frames, %s)\n", outMs, out.frames,
                out.measured ? "the platform's own figure, its queue included"
                             : "the app's buffer alone, read back from the device");
    std::printf("    ------------------------------------------------------------------\n");
    if (out.measured) {
        std::printf("    total                           %5.2f ms — every term accounted for, and the\n",
                    pollMs + blockMean / 2.0 + outMs);
        std::printf("    last one is still the platform's claim about itself. One ptlat recording settles it.\n");
    } else {
        std::printf("    at least                        %5.2f ms, and the driver's own queue is NOT in\n",
                    pollMs + blockMean / 2.0 + outMs);
        std::printf("    that sum — it is the term ptlat exists for. Nothing in this process can hear\n"
                    "    its own speaker.\n");
    }
    std::fflush(stdout);
}

}  // namespace latency

#endif  // POCKETTRACKER_LATENCY_PROBE_H
