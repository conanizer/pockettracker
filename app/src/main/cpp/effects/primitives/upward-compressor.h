#pragma once
#include <cmath>
#include "daisysp/dsp.h"

// ===========================================================================
// UpwardCompressor — boosts signals below threshold toward the threshold.
// Modeled on DaisySP Compressor (LGPL-2.1) with inverted gain curve.
//
// DaisySP downward:  gain_rec += ratioMul × max(envDb − thresh, 0)  → reduces
// Upward (this):     gain_rec += ratioMul × max(thresh − envDb, 0)  → boosts
//
// API matches DaisySP: Process(key) updates gain, Apply(in) applies it.
// Use linked stereo: call Process(max(|L|,|R|)), then Apply(L) and Apply(R).
//
// Gain is computed every GAIN_PERIOD samples (pow10f is expensive), then
// smoothed per-sample with a first-order IIR (GAIN_SMOOTH). The smoother
// prevents the onset pop that occurs when gainRec over-accumulates during
// the slopeRec attack ramp: without it, the first block-rate gain_ update
// fires with ~7dB of accumulated boost before slopeRec has settled.
// ===========================================================================
struct UpwardCompressor {
    float slopeRec   = 0.f;
    float gainRec    = 0.f;
    float targetGain = 1.0f;
    float gain_      = 1.0f;
    float atkSlo     = 0.f;
    float atkSlo2    = 0.f;
    float relSlo     = 0.f;
    float threshDb   = -20.f;
    float ratioMul   = 0.f;

    static constexpr int   GAIN_PERIOD = 8;
    // ~7ms smoothing at 44.1kHz — spreads the block-rate gain jump over enough
    // samples to eliminate the onset pop caused by gainRec over-accumulation
    // during the slopeRec attack ramp (see comment above struct).
    static constexpr float GAIN_SMOOTH = 0.970f;
    int gainCounter = 0;

    void init(float sr) {
        setParams(-20.f, 3.f, 0.001f, 0.1f, sr);
    }

    void setParams(float thresh, float ratio, float atkSec, float relSec, float sr) {
        threshDb = thresh;
        atkSlo   = expf(-(1.0f / (sr * atkSec)));
        atkSlo2  = expf(-(2.0f / (sr * atkSec)));
        relSlo   = expf(-(1.0f / (sr * relSec)));
        ratioMul = (1.0f - atkSlo2) * (1.0f - 1.0f / ratio);
    }

    void Process(float key) {
        float inAbs  = fabsf(key);
        float curSlo = (slopeRec > inAbs) ? relSlo : atkSlo;
        slopeRec = slopeRec * curSlo + (1.0f - curSlo) * inAbs;

        if (slopeRec < 1e-6f) {
            // During true silence: stop accumulating, let gain_ decay smoothly
            // toward 1.0. Keeps gain_ stable between notes (no abrupt resets).
            gainCounter = 0;
            gain_ = gain_ * GAIN_SMOOTH + 1.0f * (1.0f - GAIN_SMOOTH);
            return;
        }

        float envDb = daisysp::fastlog10f(slopeRec) * 20.f;
        gainRec = atkSlo2 * gainRec + ratioMul * fmaxf(threshDb - envDb, 0.f);

        if (++gainCounter >= GAIN_PERIOD) {
            targetGain  = daisysp::pow10f(0.05f * gainRec);
            gainCounter = 0;
        }
        // Smooth every sample to eliminate the block-rate onset pop.
        gain_ = gain_ * GAIN_SMOOTH + targetGain * (1.0f - GAIN_SMOOTH);
    }

    float Apply(float in) const { return gain_ * in; }

    void reset() {
        slopeRec = 0.f; gainRec = 0.f;
        targetGain = 1.0f; gain_ = 1.0f;
        gainCounter = 0;
    }
};
