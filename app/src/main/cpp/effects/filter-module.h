#pragma once
#include "../filter.h"  // calculateBiquadCoeffs

// ===========================================================================
// BiquadState — DSP primitive.
// Holds per-channel history for a single biquad filter stage.
// Coefficients are owned by FilterModule; this struct holds only state.
// ===========================================================================
struct BiquadState {
    float x1=0, x2=0, y1=0, y2=0;

    void reset() { x1=x2=y1=y2=0; }

    inline float process(float in, float b0, float b1, float b2, float a1, float a2) {
        float y = b0*in + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2=x1; x1=in; y2=y1; y1=y;
        return y;
    }
};

// ===========================================================================
// FilterModule — resonant filter with dual biquad state (mono + stereo ready).
// Wraps BiquadState and calculateBiquadCoeffs into the DSP module layer.
//
// Usage:
//   filter.setParams(type, cutoff, resonance, sampleRate);   // once per block
//   sample = filter.processMono(sample);                     // per sample, sampler
//   filter.processStereo(L, R);                              // per sample, SF
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
    BiquadState stateL;   // mono or left channel state
    BiquadState stateR;   // right channel state (stereo SF voices)

    void reset() {
        type = 0;
        b0=1; b1=b2=a1=a2=0;
        stateL.reset();
        stateR.reset();
    }

    // Recompute biquad coefficients. Call once per audio block when params change.
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
