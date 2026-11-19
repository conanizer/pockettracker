#pragma once
#include <cmath>
#include "../mod-system.h"

// One block of a straight ADSR slot. `rMult` is the mod-to-mod rate (1 = none).
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
        case 4: // Release: ramp releaseFrom → 0
            if (effRelease > 0) {
                mod.envValue = fmaxf(0.0f,
                    mod.releaseFrom * (1.0f - (float)mod.stageCounter / effRelease));
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

// The curved stages: the attack charges toward ENV_OVERSHOOT and stops at 1, so it bends over near the
// top. A FAST fall is exponential, 60 dB in its time, and snaps to its end there. An ANALOG fall aims
// ENV_UNDERSHOOT below its end and stops on it, so it bends far less and lands exactly in its time.
inline constexpr float ENV_OVERSHOOT  = 1.3f;
inline constexpr float ENV_ATTACK_K   = 1.4663371f;   // ln(1.3 / 0.3): the charge reaches 1 in the attack time
inline constexpr float ENV_FALL_K     = 6.9077553f;   // ln(1000)
inline constexpr float ENV_UNDERSHOOT = 0.0523957f;   // 1 / (e³ − 1)
inline constexpr float ENV_ANALOG_K   = 3.0f;         // ln(1.0524 / 0.0524): a full 1 → 0 fall lands in its time

// How far one step of a curved stage goes: expf(−K × frames / the stage's length). 0 for a stage of no
// length — the steps below read it as "there at once".
inline float env_factor(float frames, float k, int samples) {
    return samples > 0 ? expf(-frames * k / samples) : 0.0f;
}

// The curved stages' factors for steps of `frames` (mod-to-mod rate included). Worked out once, so a
// voice can step its VOL envelope every sample without an expf on each.
struct EnvFactors { float attack = 0.0f, decay = 0.0f, release = 0.0f; };
inline EnvFactors env_factors(const VoiceModSlot& mod, float frames, bool analog) {
    const float fall = analog ? ENV_ANALOG_K : ENV_FALL_K;
    return { env_factor(frames, ENV_ATTACK_K, mod.attackSamples),
             env_factor(frames, fall, mod.decaySamples),
             env_factor(frames, fall, mod.releaseSamples) };
}

// One step of a fall from wherever `v` is to `end`; true once it is there.
inline bool envFall(float& v, float end, float factor, bool analog) {
    if (analog) {
        const float target = end - ENV_UNDERSHOOT;
        v = target + (v - target) * factor;
        if (v <= end) { v = end; return true; }
        return false;
    }
    v = end + (v - end) * factor;
    if (fabsf(v - end) < 0.001f) { v = end; return true; }
    return false;
}

// One step of the curved attack (stage 1); true once it has reached 1.
inline bool envRise(float& v, float factor) {
    v = ENV_OVERSHOOT - (ENV_OVERSHOOT - v) * factor;
    if (v < 1.0f) return false;
    v = 1.0f;
    return true;
}

// One step of a curved ADSR or an ENV slot. A release starts from wherever the value is.
inline void tickENV(VoiceModSlot& mod, const EnvFactors& f, bool analog) {
    switch (mod.stage) {
        case 1:
            if (envRise(mod.envValue, f.attack)) mod.stage = 2;
            break;
        case 2:
            if (envFall(mod.envValue, mod.sustainLevel, f.decay, analog)) mod.stage = 3;
            break;
        case 3:
            mod.envValue = mod.sustainLevel;
            break;
        case 4:
            if (envFall(mod.envValue, 0.0f, f.release, analog)) mod.stage = 5;
            break;
        default:
            break;
    }
}

// One step of `numFrames` of a curved AHD slot: the curved attack, the hold, then a fall to 0. Stages
// as tickAHD's.
inline void tickAHDCurved(VoiceModSlot& mod, const EnvFactors& f, int numFrames, float rMult, bool analog) {
    switch (mod.stage) {
        case 1:
            if (envRise(mod.envValue, f.attack)) { mod.stage = 2; mod.stageCounter = 0; }
            break;
        case 2:
            mod.envValue = 1.0f;
            mod.stageCounter += numFrames;
            if (mod.stageCounter >= static_cast<int>(mod.holdSamples / rMult)) mod.stage = 3;
            break;
        case 3:
            if (envFall(mod.envValue, 0.0f, f.decay, analog)) mod.stage = 4;
            break;
        default:
            break;
    }
}
