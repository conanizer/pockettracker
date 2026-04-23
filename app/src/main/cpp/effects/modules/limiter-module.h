#pragma once
#include "../primitives/daisysp/limiter.h"

// ===========================================================================
// LimiterModule — peak-tracking soft limiter for the master output bus.
//
// DaisySP Limiter by Emilie Gillet / Mutable Instruments (MIT).
// Peak tracker: fast attack (α=0.05), slow release (α=0.00002).
// Output: SoftLimit(pre * gain * 0.7) — Padé rational soft saturation.
//   The 0.7 factor is baked into the DaisySP implementation and keeps the
//   ceiling well below 0 dBFS; loud transients are shaped rather than
//   hard-clipped.
//
// Two independent Limiter instances (L and R) to operate on interleaved
// stereo without deinterleave allocation. Per-sample ProcessBlock(ptr, 1)
// calls are equivalent to the mono block API at block granularity.
//
// preGain: input gain applied before peak detection (default 1.0).
//   Raise above 1.0 to drive the limiter harder; the DaisySP peak tracker
//   compensates automatically so the output ceiling stays safe.
// ===========================================================================
struct LimiterModule {
    daisysp::Limiter limL, limR;
    float preGain = 1.0f;

    void reset() {
        limL.Init();
        limR.Init();
    }

    void setPreGain(float g) { preGain = g; }

    void process(float* buf, int numFrames, int channelCount) {
        for (int i = 0; i < numFrames; i++) {
            limL.ProcessBlock(&buf[i * channelCount],     1, preGain);
            limR.ProcessBlock(&buf[i * channelCount + 1], 1, preGain);
        }
    }
};
