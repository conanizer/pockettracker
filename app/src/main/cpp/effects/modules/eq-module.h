#pragma once
#include <cmath>
#include "../soundpipe/soundpipe.h"

// ===========================================================================
// EqModule — 3-band parametric EQ using Soundpipe pareq.
//
// Band types (match Kotlin EqBand.type):
//   0 = off (bypass)
//   1 = loShelf   (pareq mode 1)
//   2 = bell      (pareq mode 0, peaking EQ)
//   3 = hiShelf   (pareq mode 2)
//
// Parameters:
//   freqHz  — centre/corner frequency, 20–20000 Hz
//   gainDb  — gain in dB, −12..+12 (ignored for off; 0 dB = unity)
//   q       — bandwidth/Q, 0.1–10.0
//
// Usage:
//   band.reset(sampleRate);
//   band.setParams(type, freqHz, gainDb, q);
//   out = band.processMono(in);          // sampler voices
//   band.processStereo(L, R);            // SF stereo voices
//
// Adding to InstrumentChain follows the existing "Adding a new module" pattern:
//   1. Add member EqModule eq.
//   2. Call eq.reset(sr) in reset().
//   3. Call eq.processMono / processStereo in the signal chain.
//   Call sites in audio-engine.cpp do not change.
// ===========================================================================

// Pareq mode index for each band type.
inline int eqTypeToPareqMode(int type) {
    if (type == 1) return 1;  // loShelf
    if (type == 3) return 2;  // hiShelf
    return 0;                 // bell (default)
}

// One EQ band: two sp_pareq instances (L and R share coefficients, separate state).
struct EqBandModule {
    sp_pareq pL, pR;
    sp_data  sp;
    bool bypass = true;

    void reset(float sr) {
        sp.sr = sr;
        sp_pareq_init(&sp, &pL);
        sp_pareq_init(&sp, &pR);
        bypass = true;
    }

    // type: 0=off 1=loShelf 2=bell 3=hiShelf
    // freqHz: 20–20000, gainDb: −12..+12, q: 0.1–10.0
    void setParams(int type, float freqHz, float gainDb, float q) {
        bypass = (type == 0);
        if (bypass) return;

        float v    = powf(10.0f, gainDb / 20.0f);  // dB → linear amplitude
        float mode = (float)eqTypeToPareqMode(type);

        pL.fc = freqHz; pL.v = v; pL.q = q; pL.mode = mode;
        pL.prv_fc = pL.prv_v = pL.prv_q = -1.0f;  // force coefficient recalc

        pR.fc = freqHz; pR.v = v; pR.q = q; pR.mode = mode;
        pR.prv_fc = pR.prv_v = pR.prv_q = -1.0f;
    }

    inline float processMono(float in) {
        if (bypass) return in;
        SPFLOAT out, s = in;
        sp_pareq_compute(&sp, &pL, &s, &out);
        return out;
    }

    inline void processStereo(float& L, float& R) {
        if (bypass) return;
        SPFLOAT outL, outR, sL = L, sR = R;
        sp_pareq_compute(&sp, &pL, &sL, &outL);
        sp_pareq_compute(&sp, &pR, &sR, &outR);
        L = outL; R = outR;
    }
};

// Three-band parametric EQ.
struct EqModule {
    EqBandModule bands[3];
    bool active = false;

    void reset(float sr) {
        for (auto& b : bands) b.reset(sr);
        active = false;
    }

    inline float processMono(float in) {
        if (!active) return in;
        in = bands[0].processMono(in);
        in = bands[1].processMono(in);
        return bands[2].processMono(in);
    }

    inline void processStereo(float& L, float& R) {
        if (!active) return;
        bands[0].processStereo(L, R);
        bands[1].processStereo(L, R);
        bands[2].processStereo(L, R);
    }
};
