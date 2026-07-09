#pragma once
#include <cmath>
#include "../mod-system.h"
#include "../primitives/lfo-oscillator.h"
#include "../../rng.h"

// tickLFO — advance one audio block for an LFO mod slot (type 3).
//   sr    : sample rate in Hz (for Hz → radians/frame conversion).
//   rMult : rate multiplier from mod-to-mod routing (1.0 = no mod).
// Updates mod.lfoPhase and mod.envValue in place.
//
// Trigger modes (lfoTrigMode): 0=FREE and 1=RETG differ only in the phase set at note
// trigger (FREE aligns to the global frame clock, RETG resets to 0 — both handled in the
// dispatch copy loop). 2=HOLD freezes the value sampled at trigger. 3=ONCE runs exactly
// one cycle, then parks at stage 4 holding the shape's end-of-cycle value (mod-runner
// still writes a stage-4 LFO's envValue to the source array each block).
//
// SCALAR (type=6): constant output equal to mod.amount — not modelled here.
// It is a degenerate LFO that never oscillates. It is handled as its own short
// branch in mod-runner.h. Whether type=6 should instead be a dedicated
// ModSourceId (static per-note scalar, like MOD_SRC_VELOCITY) is unresolved;
// left as-is until the feature is actually used. — TODO: revisit before shipping.
inline void tickLFO(VoiceModSlot& mod, int numFrames, float sr, float rMult) {
    if (mod.lfoTrigMode == 2 || mod.stage == 4) return;  // HOLD / finished ONCE: value frozen

    float effHz        = mod.lfoHz * rMult;
    float phaseAdvance = 2.0f * (float)M_PI * effHz / sr * numFrames;
    mod.lfoPhase += phaseAdvance;
    bool wrapped = false;
    while (mod.lfoPhase >= 2.0f * (float)M_PI) { mod.lfoPhase -= 2.0f * (float)M_PI; wrapped = true; }

    if (mod.oscShape == 8 || mod.oscShape == 9) {
        // RND = sample & hold (new random level each cycle), DRNK = clamped random walk.
        // Stateful — can't live in the pure lfoShape() map. RNG state is per-slot,
        // seeded at note trigger; ONCE keeps its trigger-time level for the whole note.
        if (wrapped && mod.lfoTrigMode != 3) {
            if (mod.lfoRngState == 0) mod.lfoRngState = 0x9E3779B9u;  // xorshift dead-state guard
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
        // A completing ONCE holds the end-of-cycle value, not the post-wrap value.
        float phase = (wrapped && mod.lfoTrigMode == 3)
            ? 2.0f * (float)M_PI - 1e-4f
            : mod.lfoPhase;
        mod.envValue = lfoShape(phase, mod.oscShape);
    }

    if (wrapped && mod.lfoTrigMode == 3) mod.stage = 4;  // ONCE: one cycle done — freeze
}
