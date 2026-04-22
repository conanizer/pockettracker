#pragma once
#include <cmath>
#include "../primitives/svf.h"

// ===========================================================================
// FilterModule — resonant SVF filter (LP / HP / BP).
// Wraps two SvfState instances with coefficient management.
//
// Usage (once per audio block when params change):
//   filter.setParams(type, cutoff, resonance, sampleRate);
//
// Usage (per sample in the mix loop):
//   sample = filter.processMono(sample);      // sampler voices (mono)
//   filter.processStereo(L, R);               // SF voices (stereo)
//
// All external call sites (audio-engine.cpp, instrument-chain.h) are
// unchanged from the biquad version. The SVF primitive (SvfState) is
// independently reusable — e.g. OTT crossover filters own their own
// SvfState pair and pass different caller-computed f/damp values.
// ===========================================================================
struct FilterModule {
    int   type = 0;
    float freq = 0.0f;   // 2*sin(pi*hz/sr)  — computed in setParams()
    float damp = 2.0f;   // SVF damping       — computed in setParams()
    SvfState stateL;     // mono or left channel
    SvfState stateR;     // right channel (SF stereo only)

    void reset() {
        type = 0;
        freq = 0.0f; damp = 2.0f;
        stateL.reset();
        stateR.reset();
    }

    // Recompute coefficients. Call once per block when type or modulated params change.
    void setParams(int filterType, int cutoff, int resonance, float sampleRate) {
        type = filterType;

        // cutoff 0-255 → Hz, exponential curve (20 Hz – 20 kHz)
        float hz = 20.0f * powf(1000.0f, cutoff / 255.0f);
        hz = fminf(hz, sampleRate * 0.45f);

        // SVF frequency coefficient
        freq = 2.0f * sinf((float)M_PI * hz / sampleRate);

        // resonance 0-255 → 0..1, then derive damping
        // high res01 → low damp → high Q; stability cap prevents blow-up near Nyquist
        float res01 = resonance / 255.0f;
        float d     = 2.0f * (1.0f - powf(res01, 0.25f));
        float maxD  = fminf(2.0f, 2.0f / freq - freq * 0.5f);
        damp = fminf(d, maxD);
    }

    bool enabled() const { return type != 0; }

    inline float processMono(float in) {
        if (!enabled()) return in;
        stateL.process(in, freq, damp);
        if (type == 1) return stateL.low();
        if (type == 2) return stateL.high();
        return stateL.band();   // type == 3
    }

    inline void processStereo(float& L, float& R) {
        if (!enabled()) return;
        stateL.process(L, freq, damp);
        stateR.process(R, freq, damp);
        if (type == 1) { L = stateL.low();  R = stateR.low();  return; }
        if (type == 2) { L = stateL.high(); R = stateR.high(); return; }
        L = stateL.band(); R = stateR.band();
    }
};
