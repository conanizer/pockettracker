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
    // ⚠️ **A WRITE ONTO A SOUNDING NOTE GLIDES, IT DOES NOT JUMP.** A cutoff, resonance or type change
    // mid-note (a table row, a phrase command, a knob) is a step in the output — a click — unless it is
    // spread over CHANGE_FRAMES. Counted in SAMPLES, not in pieces: a piece can end a few samples after
    // the write, at the block's edge, and a ramp that long is still a click. ⚠️ Everything written
    // before the note's FIRST SAMPLE lands at once (`fresh`): the trigger's set-up, a filter an INS row
    // carried, a table's first row. Faded in, those would let the attack through unfiltered.
    static constexpr int CHANGE_FRAMES = 128;

    int   type       = 0;
    int   drive      = 150;
    float sampleRate = 44100.0f;
    daisysp::Svf svfL;   // mono or left channel
    daisysp::Svf svfR;   // right channel (SF stereo only)

    // Per-block interpolation: prev* = start-of-block, target* = end-of-block
    float prevFreqL  = 0.25f, prevDampL  = 0.0f;
    float targetFreqL = 0.25f, targetDampL = 0.0f;
    float prevFreqR  = 0.25f, prevDampR  = 0.0f;
    float targetFreqR = 0.25f, targetDampR = 0.0f;

    bool  fresh = true;
    // The coefficients the last sample used, and a glide away from them.
    float lastFreqL = 0.25f, lastDampL = 0.0f, lastFreqR = 0.25f, lastDampR = 0.0f;
    float fromFreqL = 0.25f, fromDampL = 0.0f, fromFreqR = 0.25f, fromDampR = 0.0f;
    int   glideLeft = 0;
    // A type change crossfades from the old type's output (0 = the dry input) to the new one's.
    int   prevType = 0;
    int   typeFadeLeft = 0;

    void reset() {
        type = 0;
        fresh = true;
        glideLeft = typeFadeLeft = 0;
        svfL.Init(sampleRate);
        svfR.Init(sampleRate);
        prevFreqL = targetFreqL = svfL.GetFreq();
        prevDampL = targetDampL = svfL.GetDamp();
        prevFreqR = targetFreqR = svfR.GetFreq();
        prevDampR = targetDampR = svfR.GetDamp();
    }

    // Snapshot start-of-block coefficients. Call BEFORE setParams each block.
    void snapshotCoeffs() {
        prevFreqL = targetFreqL; prevDampL = targetDampL;
        prevFreqR = targetFreqR; prevDampR = targetDampR;
    }

    // Recompute all parameters. Call once per block when any param changes.
    void setParams(int filterType, int cutoff, int resonance, int filterDrive, float sr) {
        const int oldType = type;
        type  = filterType;
        drive = filterDrive;
        if (sr != sampleRate) {
            sampleRate = sr;
            svfL.Init(sr);
            svfR.Init(sr);
        } else if (!fresh && oldType == 0 && filterType != 0) {
            // Switched on mid-note: its integrators hold whatever they held when it was last on.
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
        const float newFreqL = svfL.GetFreq(), newDampL = svfL.GetDamp();
        const float newFreqR = svfR.GetFreq(), newDampR = svfR.GetDamp();
        if (!fresh && oldType != 0 && filterType != 0 &&
            (newFreqL != targetFreqL || newDampL != targetDampL)) {
            fromFreqL = lastFreqL; fromDampL = lastDampL;
            fromFreqR = lastFreqR; fromDampR = lastDampR;
            glideLeft = CHANGE_FRAMES;
        }
        targetFreqL = newFreqL; targetDampL = newDampL;
        targetFreqR = newFreqR; targetDampR = newDampR;
        if (oldType == 0 && filterType != 0) {   // nothing to glide from: start where it is set
            prevFreqL = lastFreqL = targetFreqL; prevDampL = lastDampL = targetDampL;
            prevFreqR = lastFreqR = targetFreqR; prevDampR = lastDampR = targetDampR;
        }
        if (!fresh && oldType != filterType) {
            prevType = oldType;
            typeFadeLeft = CHANGE_FRAMES;
        }
    }

    // Per-sample coefficient interpolation. Call inside the mix loop before process*.
    // Linearly blends prev→target coefficients without expensive trig recalculation.
    inline void setInterpolatedCoeffs(float t) {
        if (!enabled() && typeFadeLeft == 0) return;
        float fL = prevFreqL + (targetFreqL - prevFreqL) * t, dL = prevDampL + (targetDampL - prevDampL) * t;
        float fR = prevFreqR + (targetFreqR - prevFreqR) * t, dR = prevDampR + (targetDampR - prevDampR) * t;
        if (glideLeft > 0) {
            const float k = static_cast<float>(glideLeft) / CHANGE_FRAMES;
            fL = fromFreqL * k + fL * (1.0f - k); dL = fromDampL * k + dL * (1.0f - k);
            fR = fromFreqR * k + fR * (1.0f - k); dR = fromDampR * k + dR * (1.0f - k);
            --glideLeft;
        }
        lastFreqL = fL; lastDampL = dL; lastFreqR = fR; lastDampR = dR;
        svfL.SetCoeffs(fL, dL);
        svfR.SetCoeffs(fR, dR);
    }

    bool enabled() const { return type != 0; }

    static inline float tap(daisysp::Svf& s, int t, float in) {
        switch (t) {
            case 1: return s.Low();
            case 2: return s.High();
            case 3: return s.Band();
            case 4: return s.Notch();
            case 5: return s.Peak();
            default: return in;
        }
    }

    inline float processMono(float in) {
        fresh = false;
        if (!enabled() && typeFadeLeft == 0) return in;
        svfL.Process(in);
        float out = tap(svfL, type, in);
        if (typeFadeLeft > 0) {
            const float k = static_cast<float>(typeFadeLeft) / CHANGE_FRAMES;
            out = tap(svfL, prevType, in) * k + out * (1.0f - k);
            --typeFadeLeft;
        }
        return out;
    }

    inline void processStereo(float& L, float& R) {
        fresh = false;
        if (!enabled() && typeFadeLeft == 0) return;
        svfL.Process(L);
        svfR.Process(R);
        float outL = tap(svfL, type, L), outR = tap(svfR, type, R);
        if (typeFadeLeft > 0) {
            const float k = static_cast<float>(typeFadeLeft) / CHANGE_FRAMES;
            outL = tap(svfL, prevType, L) * k + outL * (1.0f - k);
            outR = tap(svfR, prevType, R) * k + outR * (1.0f - k);
            --typeFadeLeft;
        }
        L = outL;
        R = outR;
    }
};
