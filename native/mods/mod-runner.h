#pragma once
#include <cstring>
#include <cmath>
#include "mod-system.h"
#include "modules/ahd-module.h"
#include "modules/adsr-module.h"
#include "modules/lfo-module.h"

// One voice's modulation for one block. ⚠️ Runs after tickPitchSlide and tickVibrato, which write
// their sources first.
inline void runModMatrix(IAudioVoice& voice, int numFrames, float sr) {
    // LFO and SCALAR slots feed the LFO sources, envelopes the ENV ones.
    auto sourceOf = [](const VoiceModSlot& mod, int m) {
        return (ModSourceId)((mod.type == 3 || mod.type == 6) ? MOD_SRC_LFO0 + m : MOD_SRC_ENV0 + m);
    };
    memcpy(voice.prevModDestValues, voice.modDestValues, sizeof(float) * PARAM_COUNT);

    // Only the envelope and LFO sources are cleared; the others were written before this.
    for (int m = 0; m < 4; m++) {
        voice.modSourceValues[MOD_SRC_ENV0 + m] = 0.0f;
        voice.modSourceValues[MOD_SRC_LFO0 + m] = 0.0f;
    }
    voice.modSourceValues[MOD_SRC_NONE] = 0.0f;

    // Mod-to-mod: a slot aimed at MOD_AMT adds to the next slot's amount, MOD_RATE multiplies its rate.
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
            mod.envValue = mod.amount;   // SCALAR: a constant
        }

        voice.modSourceValues[sourceOf(mod, m)] = mod.envValue;
    }

    // Up to 4 slot routes, rebuilt every block (mod-to-mod moves the amounts), plus 4 fixed ones.
    // VOL is applied per sample in the mix loop; dest ≥ 7 is mod-to-mod.
    ModRoute routes[8];
    int routeCount = 0;

    for (int m = 0; m < 4; m++) {
        const VoiceModSlot& mod = voice.voiceMods[m];
        if (mod.type == 0) continue;
        if (mod.dest == 0 || mod.dest == 1) continue;
        if (mod.dest >= 7) continue;

        const ModSourceId srcId = sourceOf(mod, m);
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

    // The fixed routes. Volume = table × phrase (the via) × instrument (the depth).
    routes[routeCount++] = { MOD_SRC_TABLE_PITCH, PARAM_PITCH, 1.0f, MOD_SRC_NONE,        0.0f };
    routes[routeCount++] = { MOD_SRC_PITCH_SLIDE, PARAM_PITCH, 1.0f, MOD_SRC_NONE,        0.0f };
    routes[routeCount++] = { MOD_SRC_VIBRATO,     PARAM_PITCH, 1.0f, MOD_SRC_NONE,        0.0f };
    routes[routeCount++] = { MOD_SRC_TABLE_VOL,   PARAM_VOL,
                              voice.params.base[PARAM_VOL],
                              MOD_SRC_PHRASE_VOL, 1.0f };

    processRoutes(voice.modSourceValues, voice.modDestValues, routes, routeCount);

    // The mix loop reads params.get().
    for (int p = 0; p < PARAM_COUNT; p++) {
        voice.params.mod[p] = voice.modDestValues[p];
    }
}
