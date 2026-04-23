#pragma once
#include <cmath>
#include "../primitives/daisysp/svf.h"

// ===========================================================================
// FilterModule — resonant SVF filter via DaisySP Svf.
//
// DaisySP Svf is a double-sampled, stable state variable filter by Andrew
// Simper (musicdsp.org), ported by Stephen Hensley. Double sampling gives
// better high-frequency accuracy; the cubic band term (drive_ * band³)
// provides nonlinear resonance stabilisation and character.
//
// Filter types:  0=off  1=LP  2=HP  3=BP  4=Notch  5=Peak
//   Notch and Peak are free: DaisySP Svf computes all outputs simultaneously.
//
// Parameters (all stored so modulation paths only need to pass changed ones):
//   type  0-5   filter type (see above)
//   cut   0-255 cutoff frequency   → 20–20 kHz exponential
//   res   0-255 resonance          → 0..1 (0=none, 255=max)
//   drive 0-255 SVF resonance saturation → SetDrive 0-10
//              128 = DaisySP Init default (pre_drive=0.5)
//
// Usage (once per block when params change):
//   filter.setParams(type, cut, res, drive, sampleRate);
//
// Usage (per sample):
//   sample = filter.processMono(sample);
//   filter.processStereo(L, R);
//
// reset() clears integrator state via Init(). setParams() always follows
// reset() at voice trigger, restoring all parameters before audio runs.
// Call sites in InstrumentChain and audio-engine.cpp are unchanged.
// ===========================================================================
struct FilterModule {
    int   type       = 0;
    int   drive      = 128;
    float sampleRate = 44100.0f;
    daisysp::Svf svfL;   // mono or left channel
    daisysp::Svf svfR;   // right channel (SF stereo only)

    void reset() {
        type = 0;
        svfL.Init(sampleRate);
        svfR.Init(sampleRate);
    }

    // Recompute all parameters. Call once per block when any param changes.
    void setParams(int filterType, int cutoff, int resonance, int filterDrive, float sr) {
        type  = filterType;
        drive = filterDrive;
        if (sr != sampleRate) {
            sampleRate = sr;
            svfL.Init(sr);
            svfR.Init(sr);
        }
        // cutoff 0-255 → Hz, exponential curve (20 Hz – 20 kHz)
        float hz  = 20.0f * powf(1000.0f, cutoff / 255.0f);
        hz = fminf(hz, sr * 0.45f);
        // resonance 0-255 → 0..1
        float res = resonance / 255.0f;
        // drive 0-255 → 0..10 (DaisySP SetDrive input range)
        float drv = filterDrive / 25.5f;
        svfL.SetFreq(hz);  svfL.SetRes(res);  svfL.SetDrive(drv);
        svfR.SetFreq(hz);  svfR.SetRes(res);  svfR.SetDrive(drv);
    }

    bool enabled() const { return type != 0; }

    inline float processMono(float in) {
        if (!enabled()) return in;
        svfL.Process(in);
        switch (type) {
            case 1: return svfL.Low();
            case 2: return svfL.High();
            case 3: return svfL.Band();
            case 4: return svfL.Notch();
            case 5: return svfL.Peak();
            default: return in;
        }
    }

    inline void processStereo(float& L, float& R) {
        if (!enabled()) return;
        svfL.Process(L);
        svfR.Process(R);
        switch (type) {
            case 1: L = svfL.Low();   R = svfR.Low();   return;
            case 2: L = svfL.High();  R = svfR.High();  return;
            case 3: L = svfL.Band();  R = svfR.Band();  return;
            case 4: L = svfL.Notch(); R = svfR.Notch(); return;
            case 5: L = svfL.Peak();  R = svfR.Peak();  return;
        }
    }
};
