#pragma once
#include <cmath>

// ===========================================================================
// SvfState — State Variable Filter, two-integrator-loop topology.
//
// Provides LP/HP/BP/notch/peak outputs simultaneously from one process() call.
// Coefficients are computed by the caller module and passed at call time —
// same design as BiquadState. Two SvfState instances share the same f/damp.
//
// Caller computes coefficients (do this in setParams(), not per sample):
//   float hz   = /* cutoff frequency in Hz, clamped to 0.45 * sampleRate */
//   float f    = 2.0f * sinf(M_PI * hz / sampleRate);
//   float res01 = /* resonance 0..1 (0 = no resonance, 1 = edge of self-oscillation) */
//   float damp = fminf(2.0f * (1.0f - powf(res01, 0.25f)),
//                      fminf(2.0f, 2.0f / f - f * 0.5f));
//
// Reuse: OTT crossover filters each own an SvfState pair (L+R) and pass their
// own crossover f/damp values. Same primitive, different caller-computed coeffs.
//
// Stability: integrators are soft-clipped each sample to prevent blow-up at
// high resonance (same approach as DaisySP Svf).
// ===========================================================================
struct SvfState {
    float lp=0, bp=0, hp=0, notch=0, peak=0;

    void reset() { lp=bp=hp=notch=peak=0.0f; }

    inline void process(float in, float f, float damp) {
        notch = in   - damp * bp;
        lp    = lp   + f    * bp;
        hp    = notch - lp;
        bp    = f    * hp   + bp;
        peak  = lp   - hp;
        // soft-clip integrators — cubic waveshaper, hard-limit at ±1
        lp = softClip(lp);
        bp = softClip(bp);
    }

    float low()   const { return lp; }
    float high()  const { return hp; }
    float band()  const { return bp; }

private:
    static inline float softClip(float x) {
        if (x >  1.0f) return  1.0f;
        if (x < -1.0f) return -1.0f;
        return 1.5f * x - 0.5f * x * x * x;
    }
};
