#pragma once
#include <cmath>

static constexpr float kLR4Pi    = 3.14159265358979f;
static constexpr float kLR4Sqrt2 = 1.41421356237f;

// ===========================================================================
// LR4Biquad — 2nd-order Butterworth biquad (Transposed Direct Form II).
// Bilinear transform, coefficients computed at runtime for any fc/sr.
// LP and HP share the same feedback (a1/a2), differ only in feedforward (b).
// ===========================================================================
struct LR4Biquad {
    float b0 = 0.f, b1 = 0.f, b2 = 0.f;
    float a1 = 0.f, a2 = 0.f;
    float s1 = 0.f, s2 = 0.f;

    void init(float fc, float sr, bool hp) {
        float K    = tanf(kLR4Pi * fc / sr);
        float K2   = K * K;
        float norm = 1.0f / (1.0f + kLR4Sqrt2 * K + K2);
        if (!hp) {
            b0 = K2 * norm;  b1 = 2.0f * K2 * norm;  b2 = K2 * norm;
        } else {
            b0 = norm;       b1 = -2.0f * norm;        b2 = norm;
        }
        a1 = 2.0f * (K2 - 1.0f) * norm;
        a2 = (1.0f - kLR4Sqrt2 * K + K2) * norm;
    }

    void clear() { s1 = s2 = 0.f; }

    inline float process(float in) {
        float y = b0 * in + s1;
        s1 = b1 * in - a1 * y + s2;
        s2 = b2 * in - a2 * y;
        return y;
    }
};

// ===========================================================================
// LR4Filter — two cascaded LR4Biquad = 4th-order Linkwitz-Riley.
// LP + HP outputs sum to flat amplitude+phase response (LR4 property).
// ===========================================================================
struct LR4Filter {
    LR4Biquad lp1, lp2;
    LR4Biquad hp1, hp2;

    void init(float fc, float sr) {
        lp1.init(fc, sr, false);  lp2.init(fc, sr, false);
        hp1.init(fc, sr, true);   hp2.init(fc, sr, true);
        lp1.clear(); lp2.clear(); hp1.clear(); hp2.clear();
    }

    inline void process(float in, float& lp, float& hp) {
        lp = lp2.process(lp1.process(in));
        hp = hp2.process(hp1.process(in));
    }
};
