#pragma once
// An EQ band's numbers, shared by the engine and the EQ editor's curve, so the picture is the sound.
#include <cmath>
#include "../soundpipe/soundpipe.h"

// Authored bytes → Hz, dB, Q. ⚠️ A table morph interpolates the bytes, so this is the one mapping.
inline float eqFreqHzFromHex(int hex) { return 20.0f * powf(1000.0f, hex / 255.0f); }
inline float eqGainDbFromHex(int hex) { return hex / 10.0f - 12.0f; }   // 0..240 = −12.0..+12.0 dB
inline float eqQFromHex(int hex)      { return 0.1f * powf(100.0f, hex / 255.0f); }

// pareq's mode for a shelf or bell band type: 0 bell, 1 low shelf, 2 high shelf.
inline int eqPareqMode(int type) { return (type == 1) ? 1 : (type == 4) ? 2 : 0; }

// A shelf or bell band's gain at `atHz`, in dB, from the coefficients `sp_pareq_compute` derives —
// the ones the engine then runs.
inline double eqPareqGainDb(int type, float freqHz, float gainDb, float q, float sr, float atHz) {
    sp_data  sp{sr};
    sp_pareq p;
    sp_pareq_init(&sp, &p);
    p.fc   = freqHz;
    p.v    = powf(10.0f, gainDb / 20.0f);
    p.q    = q;
    p.mode = static_cast<SPFLOAT>(eqPareqMode(type));
    SPFLOAT in = 0.0f, out = 0.0f;
    sp_pareq_compute(&sp, &p, &in, &out);

    // H(z) = (b0 + b1 z⁻¹ + b2 z⁻²) / (1 + a1 z⁻¹ + a2 z⁻²), at z = e^{jω}.
    const double w  = 6.283185307179586 * atHz / sr;
    const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
    const double nRe = p.b0 + p.b1 * c1 + p.b2 * c2, nIm = -(p.b1 * s1 + p.b2 * s2);
    const double dRe = 1.0 + p.a1 * c1 + p.a2 * c2, dIm = -(p.a1 * s1 + p.a2 * s2);
    const double magSq = (nRe * nRe + nIm * nIm) / (dRe * dRe + dIm * dIm + 1e-30);
    return 10.0 * std::log10(magSq + 1e-30);
}
