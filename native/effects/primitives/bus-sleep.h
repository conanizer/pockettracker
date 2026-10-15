#pragma once
#include <cmath>

// A send bus's idle gate: after input and output stay under FLOOR for `quietSpan` frames the module
// stops running until the input rises again. Its state is kept, not cleared.
// ⚠️ The span must cover the longest a sound can sit in the module unheard — a whole delay line.
struct BusSleep {
    // −90 dBFS, where the offline render ends a tail.
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

    // First in `process`: true = skip, the caller writes silence. A loud input wakes it.
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
