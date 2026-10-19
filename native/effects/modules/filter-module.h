#pragma once
#include <cmath>
#include "../glide.h"
#include "../primitives/daisysp/svf.h"

// A voice's resonant filter (DaisySP's state-variable filter, all outputs at once).
// Types: 0 off, 1 LP, 2 HP, 3 BP, 4 notch, 5 peak. Cutoff 0-255 → 20 Hz–20 kHz exponential,
// resonance 0-255 → 0..1, drive 0-255 → SetDrive 0..10. `setParams` once per block when one changes.
struct FilterModule {
    // The glide rule (glide.h): `fresh` until the note's first sample.
    int   type       = 0;
    int   drive      = 150;
    float sampleRate = 44100.0f;
    daisysp::Svf svfL;   // mono or left channel
    daisysp::Svf svfR;   // right channel (SF stereo only)

    // The block's start and end coefficients, interpolated per sample.
    float prevFreqL  = 0.25f, prevDampL  = 0.0f;
    float targetFreqL = 0.25f, targetDampL = 0.0f;
    float prevFreqR  = 0.25f, prevDampR  = 0.0f;
    float targetFreqR = 0.25f, targetDampR = 0.0f;

    bool  fresh = true;
    // The coefficients the last sample used, and a glide away from them: their share, 1 to 0.
    float lastFreqL = 0.25f, lastDampL = 0.0f, lastFreqR = 0.25f, lastDampR = 0.0f;
    float fromFreqL = 0.25f, fromDampL = 0.0f, fromFreqR = 0.25f, fromDampR = 0.0f;
    Ramp  glide;
    // A type change crossfades from the old type's output (0 = the dry input) to the new one's.
    int   prevType = 0;
    Ramp  typeFade;

    void reset() {
        type = 0;
        fresh = true;
        glide = typeFade = Ramp{};
        svfL.Init(sampleRate);
        svfR.Init(sampleRate);
        prevFreqL = targetFreqL = svfL.GetFreq();
        prevDampL = targetDampL = svfL.GetDamp();
        prevFreqR = targetFreqR = svfR.GetFreq();
        prevDampR = targetDampR = svfR.GetDamp();
    }

    // Call before setParams each block.
    void snapshotCoeffs() {
        prevFreqL = targetFreqL; prevDampL = targetDampL;
        prevFreqR = targetFreqR; prevDampR = targetDampR;
    }

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
        float hz  = 20.0f * powf(1000.0f, cutoff / 255.0f);
        hz = fminf(hz, sr * 0.45f);
        float res = resonance / 255.0f;
        float drv = filterDrive / 25.5f;
        svfL.SetFreq(hz);  svfL.SetRes(res);  svfL.SetDrive(drv);
        svfR.SetFreq(hz);  svfR.SetRes(res);  svfR.SetDrive(drv);
        const float newFreqL = svfL.GetFreq(), newDampL = svfL.GetDamp();
        const float newFreqR = svfR.GetFreq(), newDampR = svfR.GetDamp();
        if (!fresh && oldType != 0 && filterType != 0 &&
            (newFreqL != targetFreqL || newDampL != targetDampL)) {
            fromFreqL = lastFreqL; fromDampL = lastDampL;
            fromFreqR = lastFreqR; fromDampR = lastDampR;
            glide.start(1.0f, 0.0f, GLIDE_SAMPLES);
        }
        targetFreqL = newFreqL; targetDampL = newDampL;
        targetFreqR = newFreqR; targetDampR = newDampR;
        if (oldType == 0 && filterType != 0) {   // nothing to glide from: start where it is set
            prevFreqL = lastFreqL = targetFreqL; prevDampL = lastDampL = targetDampL;
            prevFreqR = lastFreqR = targetFreqR; prevDampR = lastDampR = targetDampR;
        }
        if (!fresh && oldType != filterType) {
            prevType = oldType;
            typeFade.start(1.0f, 0.0f, GLIDE_SAMPLES);
        }
    }

    // Per sample, before process*: blends the coefficients without recomputing them.
    inline void setInterpolatedCoeffs(float t) {
        if (!enabled() && !typeFade.moving()) return;
        float fL = prevFreqL + (targetFreqL - prevFreqL) * t, dL = prevDampL + (targetDampL - prevDampL) * t;
        float fR = prevFreqR + (targetFreqR - prevFreqR) * t, dR = prevDampR + (targetDampR - prevDampR) * t;
        if (glide.moving()) {
            const float k = glide.next();
            fL = fromFreqL * k + fL * (1.0f - k); dL = fromDampL * k + dL * (1.0f - k);
            fR = fromFreqR * k + fR * (1.0f - k); dR = fromDampR * k + dR * (1.0f - k);
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
        if (!enabled() && !typeFade.moving()) return in;
        svfL.Process(in);
        float out = tap(svfL, type, in);
        if (typeFade.moving()) {
            const float k = typeFade.next();
            out = tap(svfL, prevType, in) * k + out * (1.0f - k);
        }
        return out;
    }

    inline void processStereo(float& L, float& R) {
        fresh = false;
        if (!enabled() && !typeFade.moving()) return;
        svfL.Process(L);
        svfR.Process(R);
        float outL = tap(svfL, type, L), outR = tap(svfR, type, R);
        if (typeFade.moving()) {
            const float k = typeFade.next();
            outL = tap(svfL, prevType, L) * k + outL * (1.0f - k);
            outR = tap(svfR, prevType, R) * k + outR * (1.0f - k);
        }
        L = outL;
        R = outR;
    }
};
