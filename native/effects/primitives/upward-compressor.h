#pragma once
#include <cmath>
#include "daisysp/dsp.h"

// Lifts signal below the threshold toward it: DaisySP's compressor with the gain curve inverted.
// Same API — Process(key) updates the gain, Apply(in) applies it. The gain is computed every
// GAIN_PERIOD samples and smoothed per sample.
// ⚠️ The boost works only in a window below the threshold (RANGE_DB, tapering over TAPER_DB).
// Unbounded, a fading tail winds it past +60 dB and the next note hits the limiter as a spike.
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
    static constexpr float GAIN_SMOOTH = 0.970f;   // ~7 ms at 44.1 kHz
    // dB under the threshold: full boost down to RANGE_DB, none past RANGE_DB + TAPER_DB.
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
            // Silence: drop the stored boost, or it spikes on the next note's first sample.
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
        gain_ = gain_ * GAIN_SMOOTH + targetGain * (1.0f - GAIN_SMOOTH);
    }

    float Apply(float in) const { return gain_ * in; }

    void reset() {
        slopeRec = 0.f; gainRec = 0.f;
        targetGain = 1.0f; gain_ = 1.0f;
        gainCounter = 0;
    }
};
