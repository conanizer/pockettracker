#pragma once
#include "../../filter.h"              // calculateBiquadCoeffs
#include "../primitives/biquad.h"

// ===========================================================================
// FilterModule — resonant biquad filter (LP / HP / BP).
// Wraps BiquadState with coefficient management.
//
// Usage (once per audio block when params change):
//   filter.setParams(type, cutoff, resonance, sampleRate);
//
// Usage (per sample in the mix loop):
//   sample = filter.processMono(sample);      // sampler voices (mono)
//   filter.processStereo(L, R);               // SF voices (stereo)
//
// Swapping to DaisySP SVF:
//   Replace BiquadState members with daisysp::Svf instances.
//   setParams() calls svf.SetFreq() / SetRes() / SetDrive().
//   processMono/processStereo call svf.Process() and read output taps.
//   Call sites in InstrumentChain do not change.
// ===========================================================================
struct FilterModule {
    int   type = 0;
    float b0=1, b1=0, b2=0, a1=0, a2=0;
    BiquadState stateL;   // mono or left channel
    BiquadState stateR;   // right channel (SF stereo only)

    void reset() {
        type = 0;
        b0=1; b1=b2=a1=a2=0;
        stateL.reset();
        stateR.reset();
    }

    // Recompute coefficients. Call once per block when type or modulated params change.
    void setParams(int filterType, int cutoff, int resonance, float sampleRate) {
        type = filterType;
        calculateBiquadCoeffs(filterType, cutoff, resonance, sampleRate, b0, b1, b2, a1, a2);
    }

    bool enabled() const { return type != 0; }

    inline float processMono(float in) {
        if (!enabled()) return in;
        return stateL.process(in, b0, b1, b2, a1, a2);
    }

    inline void processStereo(float& L, float& R) {
        if (!enabled()) return;
        L = stateL.process(L, b0, b1, b2, a1, a2);
        R = stateR.process(R, b0, b1, b2, a1, a2);
    }
};
