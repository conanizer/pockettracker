#pragma once
#include <cstring>
#include <cmath>
#include "mod-system.h"
#include "modules/ahd-module.h"
#include "modules/adsr-module.h"
#include "modules/lfo-module.h"

// runModMatrix — full modulation block update for one voice.
// Replaces the body of AudioEngine::updateVoiceModulation().
//
// Call order within processAudioBlock must not change:
//   1. tickPitchSlide + tickVibrato  (write PITCH_SLIDE + VIBRATO into modSourceValues[])
//   2. runModMatrix                  (reads all sources, advances ENV/LFO, calls processRoutes)
inline void runModMatrix(IAudioVoice& voice, int numFrames, float sr) {
    // Step 1: Snapshot previous dest values for sub-block interpolation.
    memcpy(voice.prevModDestValues, voice.modDestValues, sizeof(float) * PARAM_COUNT);

    // Step 2: Clear dynamic source slots (ENV0-3, LFO0-3) before each block.
    // Static sources (VELOCITY/KEYTRACK/RANDOM) keep their note-on values.
    // Sequencer sources (TABLE_VOL/PITCH/PITCH_SLIDE/VIBRATO) are written by their
    // state machines earlier in processAudioBlock and must NOT be cleared here.
    for (int m = 0; m < 4; m++) {
        voice.modSourceValues[MOD_SRC_ENV0 + m] = 0.0f;
        voice.modSourceValues[MOD_SRC_LFO0 + m] = 0.0f;
    }
    voice.modSourceValues[MOD_SRC_NONE] = 0.0f;  // Always 0 — required for via=NONE paths.

    // Step 3: Mod-to-mod routing — compute effectiveAmt / effectiveRateMult per slot.
    // AMT is additive: modulator contributes +norm*src.amount to the target's base amount.
    // RATE is multiplicative (frequency scaling is naturally exponential).
    {
        float amtOffset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float rateMult[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
        for (int m = 0; m < 4; m++) {
            const VoiceModSlot& src = voice.voiceMods[m];
            if (src.type == 0 || src.stage == 0) continue;
            if (src.dest != 8 && src.dest != 9 && src.dest != 10) continue;
            int target = (m + 1) % 4;
            float norm = (src.type == 3) ? (src.envValue * 0.5f + 0.5f)
                                          : fmaxf(0.0f, src.envValue);
            if (src.dest == 8 || src.dest == 10) amtOffset[target] += norm * src.amount;
            if (src.dest == 9 || src.dest == 10) {
                float rateScale = fminf(2.0f, norm * src.amount * 2.0f);
                rateMult[target] *= fmaxf(0.05f, rateScale);
            }
        }
        for (int m = 0; m < 4; m++) {
            voice.voiceMods[m].effectiveAmt      = fminf(1.0f, voice.voiceMods[m].amount + amtOffset[m]);
            voice.voiceMods[m].effectiveRateMult = rateMult[m];
        }
    }

    // Step 4: Tick each active mod slot and write envValue to the source array.
    for (int m = 0; m < 4; m++) {
        VoiceModSlot& mod = voice.voiceMods[m];
        if (mod.type == 0 || mod.stage == 0) continue;

        float rMult = fmaxf(0.01f, mod.effectiveRateMult);

        if (mod.type == 1 || mod.type == 4) {
            tickAHD(mod, numFrames, rMult);
        } else if (mod.type == 2 || mod.type == 5) {
            tickADSR(mod, numFrames, rMult);
        } else if (mod.type == 3) {
            tickLFO(mod, numFrames, sr, rMult);
        } else if (mod.type == 6) {
            // SCALAR: constant output — no state advance.
            // mod.amount is the fixed value set at note-on; stage=1 keeps this branch active.
            mod.envValue = mod.amount;
        }

        // Step 5: Write envValue to the source array.
        // VOL (dest=1) is handled per-sample in the mix loop via prevEnvValue.
        // MOD_* (dest≥7) are handled by the mod-to-mod system above.
        // SCALAR (type=6) reuses the LFO slot — degenerate LFO that never oscillates.
        ModSourceId srcId = (mod.type == 3 || mod.type == 6)
            ? (ModSourceId)(MOD_SRC_LFO0 + m)
            : (ModSourceId)(MOD_SRC_ENV0 + m);
        voice.modSourceValues[srcId] = mod.envValue;
    }

    // Step 6: Build routes array (user routes + 4 fixed sequencer routes).
    // Capacity: 4 user routes + 4 fixed sequencer routes = 8 total.
    // User routes are rebuilt each block because effectiveAmt changes with mod-to-mod.
    // VOL (dest=1) and MOD_* (dest≥7) are excluded from user routes.
    ModRoute routes[8];
    int routeCount = 0;

    for (int m = 0; m < 4; m++) {
        const VoiceModSlot& mod = voice.voiceMods[m];
        if (mod.type == 0) continue;
        if (mod.dest == 0 || mod.dest == 1) continue;  // NONE or VOL (per-sample path)
        if (mod.dest >= 7) continue;                    // STA / MOD_AMT / MOD_RATE / MOD_BOTH

        ModSourceId srcId = (mod.type == 3 || mod.type == 6)
            ? (ModSourceId)(MOD_SRC_LFO0 + m)
            : (ModSourceId)(MOD_SRC_ENV0 + m);
        ParamId destId;
        float   scale;
        switch (mod.dest) {
            case 2: destId = PARAM_PAN;        scale = mod.effectiveAmt * 0.5f;   break;
            case 3: destId = PARAM_PITCH;      scale = mod.effectiveAmt * 12.0f;  break;
            case 4: destId = PARAM_PITCH;      scale = mod.effectiveAmt * 1.0f;   break;
            case 5: destId = PARAM_FILTER_CUT; scale = mod.effectiveAmt * 255.0f; break;
            case 6: destId = PARAM_FILTER_RES; scale = mod.effectiveAmt * 255.0f; break;
            default: continue;
        }
        routes[routeCount++] = { srcId, destId, scale, MOD_SRC_NONE, 0.0f };
    }

    // Fixed routes: always-on connections from sequencer sources.
    // TABLE_PITCH and PITCH_SLIDE add semitones; VIBRATO adds ±depth semitones.
    // TABLE_VOL × PHRASE_VOL × instrVol via-based multiplication (Surge XT pattern).
    routes[routeCount++] = { MOD_SRC_TABLE_PITCH, PARAM_PITCH, 1.0f, MOD_SRC_NONE,        0.0f };
    routes[routeCount++] = { MOD_SRC_PITCH_SLIDE, PARAM_PITCH, 1.0f, MOD_SRC_NONE,        0.0f };
    routes[routeCount++] = { MOD_SRC_VIBRATO,     PARAM_PITCH, 1.0f, MOD_SRC_NONE,        0.0f };
    routes[routeCount++] = { MOD_SRC_TABLE_VOL,   PARAM_VOL,
                              voice.params.base[PARAM_VOL],   // depth = instrVol
                              MOD_SRC_PHRASE_VOL, 1.0f };     // via = phraseVol (full multiplication)

    // Step 7: Run the routing matrix.
    processRoutes(voice.modSourceValues, voice.modDestValues, routes, routeCount);

    // Step 8: Bridge — copy modDestValues into params.mod[] so existing mix-loop reads
    // of params.get(PARAM_PAN/PITCH/FILTER_CUT/FILTER_RES) continue to work unchanged.
    for (int p = 0; p < PARAM_COUNT; p++) {
        voice.params.mod[p] = voice.modDestValues[p];
    }
}
