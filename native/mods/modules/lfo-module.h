#pragma once
#include <cmath>
#include "../mod-system.h"
#include "../primitives/lfo-oscillator.h"
#include "../../rng.h"

// One block of an LFO slot. `rMult` is the mod-to-mod rate (1 = none).
// Trigger modes: 0 FREE and 1 RETG differ only in the phase set at trigger; 2 HOLD freezes the
// value taken at trigger; 3 ONCE runs one cycle and holds its end value (stage 4).
inline void tickLFO(VoiceModSlot& mod, int numFrames, float sr, float rMult) {
    if (mod.lfoTrigMode == 2 || mod.stage == 4) return;  // HOLD / finished ONCE: value frozen

    float effHz        = mod.lfoHz * rMult;
    float phaseAdvance = 2.0f * (float)M_PI * effHz / sr * numFrames;
    mod.lfoPhase += phaseAdvance;
    bool wrapped = false;
    while (mod.lfoPhase >= 2.0f * (float)M_PI) { mod.lfoPhase -= 2.0f * (float)M_PI; wrapped = true; }

    if (mod.oscShape == 8 || mod.oscShape == 9) {
        // RND: a new level each cycle. DRNK: a clamped random walk. ONCE keeps its trigger level.
        if (wrapped && mod.lfoTrigMode != 3) {
            if (mod.lfoRngState == 0) mod.lfoRngState = 0x9E3779B9u;  // xorshift sticks at 0
            float r = xorshift32Bipolar(mod.lfoRngState);
            if (mod.oscShape == 8) {
                mod.lfoRandValue = r;
            } else {
                mod.lfoRandValue += r * 0.5f;
                if (mod.lfoRandValue >  1.0f) mod.lfoRandValue =  1.0f;
                if (mod.lfoRandValue < -1.0f) mod.lfoRandValue = -1.0f;
            }
        }
        mod.envValue = mod.lfoRandValue;
    } else {
        // A finishing ONCE holds the cycle's end, not the wrapped start.
        float phase = (wrapped && mod.lfoTrigMode == 3)
            ? 2.0f * (float)M_PI - 1e-4f
            : mod.lfoPhase;
        mod.envValue = lfoShape(phase, mod.oscShape);
    }

    if (wrapped && mod.lfoTrigMode == 3) mod.stage = 4;
}
