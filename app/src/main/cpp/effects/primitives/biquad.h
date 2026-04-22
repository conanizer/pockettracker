#pragma once

// ===========================================================================
// BiquadState — DSP primitive.
// Per-channel history for one biquad filter stage (Direct Form I).
// Coefficients are computed externally and passed to process() each call.
// ===========================================================================
struct BiquadState {
    float x1=0, x2=0, y1=0, y2=0;

    void reset() { x1=x2=y1=y2=0; }

    inline float process(float in, float b0, float b1, float b2, float a1, float a2) {
        float y = b0*in + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2=x1; x1=in; y2=y1; y1=y;
        return y;
    }
};
