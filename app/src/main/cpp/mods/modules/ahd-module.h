#pragma once
#include <cmath>
#include "../mod-system.h"

// tickAHD — advance one audio block for an AHD or DRUM mod slot (type 1 or 4).
//   rMult : effective rate multiplier from mod-to-mod routing (1.0 = no mod).
// Stages: 1=Attack (0→1), 2=Hold (stay at 1), 3=Decay (1→0), 4=done.
// Updates mod.envValue, mod.stage, mod.stageCounter in place.
inline void tickAHD(VoiceModSlot& mod, int numFrames, float rMult) {
    if (mod.stage == 4) return;
    mod.stageCounter += numFrames;
    int effAttack = mod.attackSamples > 0 ? (int)fmaxf(1.0f, mod.attackSamples / rMult) : 0;
    int effHold   = mod.holdSamples   > 0 ? (int)fmaxf(1.0f, mod.holdSamples   / rMult) : 0;
    int effDecay  = mod.decaySamples  > 0 ? (int)fmaxf(1.0f, mod.decaySamples  / rMult) : 0;
    switch (mod.stage) {
        case 1: // Attack: ramp 0 → 1
            if (effAttack > 0) {
                mod.envValue = fminf(1.0f, (float)mod.stageCounter / effAttack);
                if (mod.stageCounter >= effAttack) {
                    mod.envValue = 1.0f;
                    mod.stage = 2; mod.stageCounter = 0;
                }
            } else {
                mod.envValue = 1.0f;
                mod.stage = 2; mod.stageCounter = 0;
            }
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
