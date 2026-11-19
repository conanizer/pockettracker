#pragma once
#include <cmath>
#include "../mod-system.h"
#include "../primitives/lfo-oscillator.h"
#include "../../rng.h"

// One block of an LFO slot. `rMult` is the mod-to-mod rate (1 = none). A finished ONCE sits at stage 4.
inline void tickLFO(VoiceModSlot& mod, int numFrames, float sr, float rMult) {
    using namespace songcore;
    if (mod.lfoTrigMode == LFO_HOLD || mod.stage == 4) return;  // value frozen

    float effHz        = mod.lfoHz * rMult;
    float phaseAdvance = 2.0f * (float)M_PI * effHz / sr * numFrames;
    mod.lfoPhase += phaseAdvance;
    bool wrapped = false;
    while (mod.lfoPhase >= 2.0f * (float)M_PI) { mod.lfoPhase -= 2.0f * (float)M_PI; wrapped = true; }

    if (mod.oscShape == LFO_RND || mod.oscShape == LFO_DRNK) {
        // RND: a new level each cycle. DRNK: a clamped random walk. ONCE keeps its trigger level.
        if (wrapped && mod.lfoTrigMode != LFO_ONCE) {
            if (mod.lfoRngState == 0) mod.lfoRngState = 0x9E3779B9u;  // xorshift sticks at 0
            float r = xorshift32Bipolar(mod.lfoRngState);
            if (mod.oscShape == LFO_RND) {
                mod.lfoRandValue = r;
            } else {
                mod.lfoRandValue += r * 0.5f;
                if (mod.lfoRandValue >  1.0f) mod.lfoRandValue =  1.0f;
                if (mod.lfoRandValue < -1.0f) mod.lfoRandValue = -1.0f;
            }
        }
        mod.envValue = lfoOut(mod, mod.lfoRandValue);
    } else {
        // A finishing ONCE holds the cycle's end, not the wrapped start.
        float phase = (wrapped && mod.lfoTrigMode == LFO_ONCE)
            ? 2.0f * (float)M_PI - 1e-4f
            : mod.lfoPhase;
        mod.envValue = lfoOut(mod, lfoShape(phase, mod.oscShape));
    }

    if (wrapped && mod.lfoTrigMode == LFO_ONCE) mod.stage = 4;
}
