#pragma once
#include <cmath>
#include "../mod-system.h"

// tickADSR — advance one audio block for an ADSR or TRIG mod slot (type 2 or 5).
//   rMult : effective rate multiplier from mod-to-mod routing (1.0 = no mod).
// Stages: 1=Attack (0→1), 2=Decay (1→sus), 3=Sustain, 4=Release (sus→0), 5=done.
// Updates mod.envValue, mod.stage, mod.stageCounter in place.
inline void tickADSR(VoiceModSlot& mod, int numFrames, float rMult) {
    if (mod.stage == 5) return;
    mod.stageCounter += numFrames;
    int effAttack  = mod.attackSamples  > 0 ? (int)fmaxf(1.0f, mod.attackSamples  / rMult) : 0;
    int effDecay   = mod.decaySamples   > 0 ? (int)fmaxf(1.0f, mod.decaySamples   / rMult) : 0;
    int effRelease = mod.releaseSamples > 0 ? (int)fmaxf(1.0f, mod.releaseSamples / rMult) : 0;
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
