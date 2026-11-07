// The app's own latency, measured rather than read: the wait for the loop to look at an input, and
// the visible half of the output buffer — naming the half only a microphone can measure.
// POCKETTRACKER_LATENCY=1 accumulates and prints the account at exit; off, it costs one cached bool.
//
// Poll and frame periods are both kept (the loop has two rates), and every number is binned by
// whether the transport ran. A global rather than on `AudioBackend`, which carries no instruments.
// ⚠️ The callback gap is SDL-only: Oboe's callback is below shell/ and cannot see this header.

#ifndef POCKETTRACKER_LATENCY_PROBE_H
#define POCKETTRACKER_LATENCY_PROBE_H

#include <SDL.h>

#include "audio-backend.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace latency {

// Min/mean/max of an interval, plus how often it ran long (`over`) and how often it did not wait at
// all (`burst`) — back-to-back callbacks are a driver handing over several buffers at once.
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
    std::atomic<int>  frames{0};        // per callback, as the device hands it over
    std::atomic<bool> sawCallback{false};

    uint64_t lastPollNs  = 0;                     // the loop thread's only — no atomic
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
    // Split around the division: counter × 1e9 overflows after a few weeks' uptime.
    static const Uint64 freq = SDL_GetPerformanceFrequency();
    const Uint64        c    = SDL_GetPerformanceCounter();
    return (c / freq) * 1000000000ull + ((c % freq) * 1000000000ull) / freq;
}

// Top of the loop, every tick; `isPlaying` picks the bin for this and the audio thread. Long = twice
// the 4 ms tick; tight = under 1 ms, a tick that did not wait (after a present that overran).
inline void poll_tick(bool isPlaying) {
    if (!enabled()) return;
    State&         s  = state();
    const uint64_t t  = now_ns();
    s.playing.store(isPlaying ? 1 : 0, std::memory_order_relaxed);
    if (s.lastPollNs != 0) {
        const uint64_t gap = t - s.lastPollNs;
        s.poll[isPlaying ? 1 : 0].add(gap, 8000000ull, 1000000ull);
        // Each stall as it happens, timestamped: the lookahead must outlast the longest. Above the
        // 33 ms a load refills at, or every load would print.
        if (isPlaying && gap > 50000000ull) {
            std::printf("latency: STALL %.1f ms while playing, at %.2f s\n", double(gap) / 1e6,
                        double(SDL_GetTicks64()) / 1000.0);
            std::fflush(stdout);
        }
    }
    s.lastPollNs = t;
}

// The ticks that draw. Long = a dropped frame; tight = two frames in one refresh, which the frame
// deadline exists to prevent.
inline void frame_tick(bool isPlaying) {
    if (!enabled()) return;
    State&         s = state();
    const uint64_t t = now_ns();
    if (s.lastFrameNs != 0) s.frame[isPlaying ? 1 : 0].add(t - s.lastFrameNs, 33000000ull, 8000000ull);
    s.lastFrameNs = t;
}

// From the audio thread. ⚠️ Relaxed atomics only — no lock, allocation or printf where the DSP runs.
// `frames` is what the device handed over, not what was asked.
inline void audio_callback(int frames, int sampleRate) {
    if (!enabled()) return;
    State&         s = state();
    const uint64_t t = now_ns();
    s.frames.store(frames, std::memory_order_relaxed);
    s.sawCallback.store(true, std::memory_order_relaxed);
    const uint64_t last = s.lastCbNs.exchange(t, std::memory_order_relaxed);
    if (last != 0) {
        const int bin = s.playing.load(std::memory_order_relaxed);
        // Long = twice the nominal block, back to back = a tenth, from the negotiated frames and rate.
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

// The account, at exit. ⚠️ It names what it did not measure: when the backend reports a floor
// (always on SDL) the sum says "at least". A bin under 200 samples is flagged as meaningless.
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

    // The buffer read back two ways — the seam asks the device, the callback counts what it got. On
    // SDL they match; a disagreement means one is not reading the device.
    if (fr > 0 && out.frames > 0 && fr != out.frames) {
        std::printf("\n  ⚠ the device disagrees with itself: the seam reports %d frames, the callback was\n"
                    "    handed %d. One of the two is not reading the device.\n", out.frames, fr);
    }

    // A waiting term is halved: a gesture lands uniformly inside the period. ⚠️ The poll is in a MIDI
    // key's path only for a pumped backend (Android).
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
