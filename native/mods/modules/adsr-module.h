#pragma once
#include <cmath>
#include "../mod-system.h"

// One block of an ADSR or TRIG slot. `rMult` is the mod-to-mod rate (1 = none).
// Stages: 1 attack, 2 decay, 3 sustain, 4 release, 5 done.
inline void tickADSR(VoiceModSlot& mod, int numFrames, float rMult) {
    if (mod.stage == 5) return;
    mod.stageCounter += numFrames;
    int effAttack  = mod.attackSamples  > 0 ? (int)fmaxf(1.0f, mod.attackSamples  / rMult) : 0;
    int effDecay   = mod.decaySamples   > 0 ? (int)fmaxf(1.0f, mod.decaySamples   / rMult) : 0;
    int effRelease = mod.releaseSamples > 0 ? (int)fmaxf(1.0f, mod.releaseSamples / rMult) : 0;
    switch (mod.stage) {
        case 1:
            tickAttack(mod, effAttack);
            break;
        case 2: // Decay: ramp 1 → sustainLevel
            if (effDecay > 0) {
                float t = fminf(1.0f, (float)mod.stageCounter / effDecay);
                mod.envValue = 1.0f - t * (1.0f - mod.sustainLevel);
                if (mod.stageCounter >= effDecay) {
                    mod.envValue = mod.sustainLevel;
                    mod.stage = 3; mod.stageCounter = 0;
                }
            } else {
                mod.envValue = mod.sustainLevel;
                mod.stage = 3; mod.stageCounter = 0;
            }
            break;
        case 3: // Sustain: hold at sustainLevel (until triggerNoteOff)
            mod.envValue = mod.sustainLevel;
            break;
        case 4: // Release: ramp sustainLevel → 0
            if (effRelease > 0) {
                mod.envValue = fmaxf(0.0f,
                    mod.sustainLevel * (1.0f - (float)mod.stageCounter / effRelease));
                if (mod.stageCounter >= effRelease) {
                    mod.envValue = 0.0f;
                    mod.stage = 5;
                }
            } else {
                mod.envValue = 0.0f;
                mod.stage = 5;
            }
            break;
    }
}
