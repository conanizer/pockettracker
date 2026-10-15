#pragma once
#include <cmath>
#include "../mod-system.h"

// One block of an AHD or DRUM slot. `rMult` is the mod-to-mod rate (1 = none).
// Stages: 1 attack, 2 hold, 3 decay, 4 done.
inline void tickAHD(VoiceModSlot& mod, int numFrames, float rMult) {
    if (mod.stage == 4) return;
    mod.stageCounter += numFrames;
    int effAttack = mod.attackSamples > 0 ? (int)fmaxf(1.0f, mod.attackSamples / rMult) : 0;
    int effHold   = mod.holdSamples   > 0 ? (int)fmaxf(1.0f, mod.holdSamples   / rMult) : 0;
    int effDecay  = mod.decaySamples  > 0 ? (int)fmaxf(1.0f, mod.decaySamples  / rMult) : 0;
    switch (mod.stage) {
        case 1:
            tickAttack(mod, effAttack);
            break;
        case 2: // Hold: stay at 1
            mod.envValue = 1.0f;
            if (effHold == 0 || mod.stageCounter >= effHold) {
                mod.stage = 3; mod.stageCounter = 0;
            }
            break;
        case 3: // Decay: ramp 1 → 0
            if (effDecay > 0) {
                mod.envValue = fmaxf(0.0f, 1.0f - (float)mod.stageCounter / effDecay);
                if (mod.stageCounter >= effDecay) {
                    mod.envValue = 0.0f;
                    mod.stage = 4;
                }
            } else {
                mod.envValue = 0.0f;
                mod.stage = 4;
            }
            break;
    }
}
