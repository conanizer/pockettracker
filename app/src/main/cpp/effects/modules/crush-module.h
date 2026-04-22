#pragma once
#include <cmath>

// ===========================================================================
// BitcrushModule — reduces bit depth to create lo-fi quantization noise.
// Stateless: no history, no primitives. Pure sample transform.
//
// param crush: 0–15 (0 = bypass, 1 = 15-bit, 15 = 1-bit)
//
// Note: Downsample (sample-rate reduction) is NOT in this module.
// For sampler voices, downsampling must happen pre-interpolation (sample
// address quantization) so it stays inline in the sampler mix loop.
// This module handles only the bit-depth reduction step.
// ===========================================================================
struct BitcrushModule {
    int crush = 0;   // 0 = bypass

    void reset() { crush = 0; }

    bool enabled() const { return crush > 0; }

    inline float processMono(float in) const {
        if (!enabled()) return in;
        int bits   = 16 - crush;
        if (bits < 1) bits = 1;
        int levels = 1 << bits;
        return floorf(in * levels) / levels;
    }

    inline void processStereo(float& L, float& R) const {
        if (!enabled()) return;
        int bits   = 16 - crush;
        if (bits < 1) bits = 1;
        int levels = 1 << bits;
        L = floorf(L * levels) / levels;
        R = floorf(R * levels) / levels;
    }
};
