#pragma once
#include <cmath>
#include "../primitives/daisysp/svf.h"

// ===========================================================================
// FilterModule — resonant SVF filter (LP / HP / BP) via DaisySP Svf.
//
// DaisySP Svf is a double-sampled, stable state variable filter by Andrew
// Simper (musicdsp.org), ported by Stephen Hensley. Double sampling gives
// better high-frequency accuracy; the cubic band term provides nonlinear
// stabilisation at high resonance instead of hard clipping.
//
// Usage (once per audio block when params change):
//   filter.setParams(type, cutoff, resonance, sampleRate);
//
// Usage (per sample in the mix loop):
//   sample = filter.processMono(sample);      // sampler voices (mono)
//   filter.processStereo(L, R);               // SF voices (stereo)
//
// reset() clears integrator state via Init(); parameters are restored by
// the setParams() call that always follows reset() at voice trigger time.
// Call sites in InstrumentChain and audio-engine.cpp are unchanged.
// ===========================================================================
struct FilterModule {
    int   type       = 0;
    float sampleRate = 44100.0f;
    daisysp::Svf svfL;   // mono or left channel
    daisysp::Svf svfR;   // right channel (SF stereo only)

    void reset() {
        type = 0;
        svfL.Init(sampleRate);
        svfR.Init(sampleRate);
    }

    // Recompute coefficients. Call once per block when type or modulated params change.
    void setParams(int filterType, int cutoff, int resonance, float sr) {
        type = filterType;
        if (sr != sampleRate) {
            sampleRate = sr;
            svfL.Init(sr);
            svfR.Init(sr);
        }
        // cutoff 0-255 → Hz, exponential curve for musical feel (20 Hz – 20 kHz)
        float hz  = 20.0f * powf(1000.0f, cutoff / 255.0f);
        hz = fminf(hz, sr * 0.45f);
        // resonance 0-255 → 0..1
        float res = resonance / 255.0f;
        svfL.SetFreq(hz);
        svfL.SetRes(res);
        svfR.SetFreq(hz);
        svfR.SetRes(res);
    }

    bool enabled() const { return type != 0; }

    inline float processMono(float in) {
        if (!enabled()) return in;
        svfL.Process(in);
        if (type == 1) return svfL.Low();
        if (type == 2) return svfL.High();
        return svfL.Band();   // type == 3
    }

    inline void processStereo(float& L, float& R) {
        if (!enabled()) return;
        svfL.Process(L);
        svfR.Process(R);
        if (type == 1) { L = svfL.Low();  R = svfR.Low();  return; }
        if (type == 2) { L = svfL.High(); R = svfR.High(); return; }
        L = svfL.Band(); R = svfR.Band();
    }
};
