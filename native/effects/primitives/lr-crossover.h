#pragma once
#include "lr4-filter.h"

// ===========================================================================
// LRCrossover — stereo 3-band Linkwitz-Riley crossover.
// Filter 1 splits at freq1 (120 Hz): low band + remainder.
// Filter 2 splits remainder at freq2 (2500 Hz): mid + high.
// Low + Mid + High reconstructs the original signal (flat amplitude + phase).
// ===========================================================================
struct LRCrossover {
    LR4Filter filt1L, filt1R;
    LR4Filter filt2L, filt2R;

    void init(float sr, float freq1 = 120.f, float freq2 = 2500.f) {
        filt1L.init(freq1, sr);  filt1R.init(freq1, sr);
        filt2L.init(freq2, sr);  filt2R.init(freq2, sr);
    }

    inline void split(float inL, float inR,
                      float& lowL, float& lowR,
                      float& midL, float& midR,
                      float& highL, float& highR) {
        float hp1L, hp1R;
        filt1L.process(inL, lowL, hp1L);
        filt1R.process(inR, lowR, hp1R);
        filt2L.process(hp1L, midL, highL);
        filt2R.process(hp1R, midR, highR);
    }
};
