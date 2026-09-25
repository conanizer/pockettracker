#pragma once
#include <cmath>

// A send bus's idle gate. Once the module's input AND output have both stayed under FLOOR for
// `quietSpan` frames it stops running and returns silence, until a block's input rises above
// FLOOR again; its state is left where it was, not cleared.
//
// ⚠️ THE SPAN MUST COVER THE LONGEST A SOUND CAN SIT INSIDE THE MODULE UNHEARD. A delay line holds
// an echo for its whole length while its output reads silence, so a span shorter than the line
// would put a stored echo to sleep. Quiet for a whole line means everything in it is quiet.
struct BusSleep {
    // -90 dBFS — the level the offline render already treats as the end of a tail.
    static constexpr float FLOOR = 3.1623e-5f;

    int  quietFrames = 0;
    bool asleep      = false;

    void reset() {
        quietFrames = 0;
        asleep      = false;
    }

    static float peak(const float* l, const float* r, int n) {
        float p = 0.0f;
        for (int i = 0; i < n; i++) {
            const float a = fabsf(l[i]), b = fabsf(r[i]);
            if (a > p) p = a;
            if (b > p) p = b;
        }
        return p;
    }

    // Called first in `process`: true = skip the block, the caller writes silence. A loud input
    // wakes the module and this block runs.
    bool skip(const float* inL, const float* inR, int n) {
        if (!asleep) return false;
        if (peak(inL, inR, n) < FLOOR) return true;
        reset();
        return false;
    }

    // Called after a block the module ran.
    void observe(const float* inL, const float* inR, const float* outL, const float* outR, int n,
                 int quietSpan) {
        if (peak(inL, inR, n) >= FLOOR || peak(outL, outR, n) >= FLOOR) {
            quietFrames = 0;
            return;
        }
        quietFrames += n;
        if (quietFrames >= quietSpan) asleep = true;
    }
};
