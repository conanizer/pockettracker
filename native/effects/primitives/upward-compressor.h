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
//
// ⚠️ THE BOOST WORKS IN A WINDOW BELOW THE THRESHOLD, NOT ALL THE WAY DOWN.
// The dB-domain accumulator is unbounded by construction — the deeper the input
// sits under the threshold the more gain it asks for — so a reverb tail decaying
// toward the silence gate winds the gain past +60 dB (x1500). The envelope needs
// ~5 ms to react, and a note that starts inside that window is multiplied by the
// tail's gain: a 70x full-scale spike into the master limiter, whose peak tracker
// then needs seconds to let go. RANGE_DB/TAPER_DB bound it: full boost down to
// RANGE_DB under, faded to nothing by RANGE_DB+TAPER_DB under, so the noise floor
// and the end of a tail are not lifted at all. Every reference OTT bounds this the
// same way — Vital clamps the band gain at +30 dB, Rui-727/OTT disengages past
// 30 dB under threshold and names the artifact it prevents.
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
    // dB below the threshold: full boost down to RANGE_DB, tapering to none by
    // RANGE_DB + TAPER_DB. With the OTT's -35 dB threshold that is full boost to
    // -65 dBFS and nothing below -80 dBFS — the same floor OttModule already
    // treats as silence. Caps the boost at (1 - 1/ratio) x RANGE_DB.
    static constexpr float RANGE_DB    = 30.f;
    static constexpr float TAPER_DB    = 15.f;
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
            // True silence: reset gainRec so stale accumulated boost can't spike on
            // the next note onset (old gainRec × atkSlo2 would fire before slopeRec
            // exits the silence gate, applying large upward gain to the first sample).
            gainRec = 0.f;
            gainCounter = 0;
            gain_ = gain_ * GAIN_SMOOTH + 1.0f * (1.0f - GAIN_SMOOTH);
            return;
        }

        float envDb = daisysp::fastlog10f(slopeRec) * 20.f;
        float under = threshDb - envDb;
        float want  = 0.f;
        if (under > 0.f) {
            want = fminf(under, RANGE_DB);
            if (under > RANGE_DB)
                want *= fmaxf(1.f - (under - RANGE_DB) / TAPER_DB, 0.f);
        }
        gainRec = atkSlo2 * gainRec + ratioMul * want;

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
