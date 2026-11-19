/*
The Korg 35 low-pass from sst-filters (Surge Synth Team, https://github.com/surge-synthesizer/sst-filters,
GPL-3.0-or-later), itself adapted from Odin 2's Korg35Filter (TheWaveWarden, GPL-3.0): `K35Filter::process_lp`
with its "Moderate Saturation" subtype, one channel in plain float instead of four in SIMD.
*/
#pragma once
#include <cmath>

// A 2-pole low-pass whose resonance loop runs through a tanh: loud input and high resonance growl rather
// than ring. ⚠️ The tanh is at ×2, so a quiet signal comes out ~6 dB louder than it went in.
struct K35Filter {
    // G = the one-pole gain, k = resonance 0.01..1.96, alpha = the loop's normalising gain.
    struct Coeffs { float G = 0.0f, k = 0.01f, alpha = 1.0f; };

    static constexpr float SATURATION = 2.0f;

    Coeffs c;
    float  lz = 0.0f, hz = 0.0f, z2 = 0.0f;   // the first low-pass, the high-pass, the second low-pass

    void reset() { lz = hz = z2 = 0.0f; }

    // Surge clamps the cutoff to 0.3 × the rate; above that the warped tangent runs away.
    static Coeffs coeffs(float freq, float res, float sr) {
        const float f  = fminf(fmaxf(freq, 5.0f), sr * 0.3f);
        const float g  = fast_tan(static_cast<float>(M_PI) * f / sr);
        const float G  = g / (1.0f + g);
        const float k  = fminf(fmaxf(res * 1.96f, 0.01f), 1.96f);
        return {G, k, 1.0f / (1.0f - k * G + k * G * G)};
    }

    inline float process(float in) {
        const float inv   = 1.0f - c.G;                // 1 / (1 + g)
        const float y1    = lpf(c.G, in, lz);
        const float s35   = c.k * inv * inv * z2 - inv * hz;
        const float u     = fast_tanh(SATURATION * c.alpha * (y1 + s35));
        const float y     = c.k * lpf(c.G, u, z2);
        lpf(c.G, y, hz);                               // the high-pass's state; its output is unused
        return y / c.k;
    }

private:
    static inline float lpf(float G, float in, float& z) {
        const float v = (in - z) * G;
        const float y = v + z;
        z = v + y;
        return y;
    }
    // Padé approximants, as sst-basic-blocks' fasttan / fasttanhSSEclamped.
    static inline float fast_tan(float x) {
        const float x2 = x * x;
        return x * (-135135.0f + x2 * (17325.0f + x2 * (-378.0f + x2))) /
               (-135135.0f + x2 * (62370.0f + x2 * (-3150.0f + 28.0f * x2)));
    }
    static inline float fast_tanh(float x) {
        x = fminf(5.0f, fmaxf(-5.0f, x));
        const float x2 = x * x;
        return x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2))) /
               (135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2)));
    }
};
