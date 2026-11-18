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
    using namespace songcore;
    // LFO and SCALAR slots feed the LFO sources, envelopes the ENV ones.
    auto sourceOf = [](const VoiceModSlot& mod, int m) {
        const bool lfo = mod.type == ModType::LFO || mod.type == ModType::SCALAR;
        return (ModSourceId)(lfo ? MOD_SRC_LFO0 + m : MOD_SRC_ENV0 + m);
    };
    memcpy(voice.prevModDestValues, voice.modDestValues, sizeof(float) * PARAM_COUNT);

    // Only the envelope and LFO sources are cleared; the others were written before this.
    for (int m = 0; m < 4; m++) {
        voice.modSourceValues[MOD_SRC_ENV0 + m] = 0.0f;
        voice.modSourceValues[MOD_SRC_LFO0 + m] = 0.0f;
    }
    voice.modSourceValues[MOD_SRC_NONE] = 0.0f;

    // Mod-to-mod: a slot aimed at MOD_AMT adds to the next slot's amount, MOD_RATE multiplies its rate
    // by 1 + source × AMT — ×1 at AMT 00 or a source at rest, up to ×2, and an LFO's lower half slows
    // it (to a twentieth at FF).
    {
        float amtOffset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float rateMult[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
        for (int m = 0; m < 4; m++) {
            const VoiceModSlot& src = voice.voiceMods[m];
            if (!mod_running(src)) continue;
            const bool toAmt  = src.dest == ModDest::MOD_AMT  || src.dest == ModDest::MOD_BOTH;
            const bool toRate = src.dest == ModDest::MOD_RATE || src.dest == ModDest::MOD_BOTH;
            if (!toAmt && !toRate) continue;
            int target = (m + 1) % 4;
            const bool lfo = src.type == ModType::LFO;
            float norm = lfo ? (src.envValue * 0.5f + 0.5f) : fmaxf(0.0f, src.envValue);
            if (toAmt) amtOffset[target] += norm * src.amount;
            if (toRate) {
                const float swing = lfo ? src.envValue : fmaxf(0.0f, src.envValue);
                rateMult[target] *= fmaxf(0.05f, 1.0f + swing * src.amount);
            }
        }
        for (int m = 0; m < 4; m++) {
            voice.voiceMods[m].effectiveAmt      = fminf(1.0f, voice.voiceMods[m].amount + amtOffset[m]);
            voice.voiceMods[m].effectiveRateMult = rateMult[m];
        }
    }

    for (int m = 0; m < 4; m++) {
        VoiceModSlot& mod = voice.voiceMods[m];
        if (!mod_running(mod)) continue;

        float rMult = fmaxf(0.01f, mod.effectiveRateMult);

        switch (mod.type) {
            case ModType::AHD:
            case ModType::DRUM:
                tickAHD(mod, numFrames, rMult);
                break;
            case ModType::ADSR:
            case ModType::TRIG:
                tickADSR(mod, numFrames, rMult);
                break;
            case ModType::LFO:
                tickLFO(mod, numFrames, sr, rMult);
                break;
            case ModType::SCALAR:   // a constant
                mod.envValue = mod.amount;
                break;
            default:
                break;
        }

        voice.modSourceValues[sourceOf(mod, m)] = mod.envValue;
    }

    // Up to 4 slot routes, rebuilt every block (mod-to-mod moves the amounts), plus 4 fixed ones.
    ModRoute routes[8];
    int routeCount = 0;

    for (int m = 0; m < 4; m++) {
        const VoiceModSlot& mod = voice.voiceMods[m];
        if (mod.type == ModType::NONE) continue;

        const ModSourceId srcId = sourceOf(mod, m);
        ParamId destId;
        float   scale;
        switch (mod.dest) {
            case ModDest::PAN:           destId = PARAM_PAN;        scale = mod.effectiveAmt * 0.5f;   break;
            case ModDest::PITCH:         destId = PARAM_PITCH;      scale = mod.effectiveAmt * 12.0f;  break;
            case ModDest::FINE_PITCH:    destId = PARAM_PITCH;      scale = mod.effectiveAmt * 1.0f;   break;
            case ModDest::FILTER_CUTOFF: destId = PARAM_FILTER_CUT; scale = mod.effectiveAmt * 255.0f; break;
            case ModDest::FILTER_RES:    destId = PARAM_FILTER_RES; scale = mod.effectiveAmt * 255.0f; break;
            // A MACRO voice's two controls; read by no other voice.
            case ModDest::TIMBRE:        destId = PARAM_MACRO_TIMBRE; scale = mod.effectiveAmt * 255.0f; break;
            case ModDest::COLOR:         destId = PARAM_MACRO_COLOR;  scale = mod.effectiveAmt * 255.0f; break;
            default: continue;   // VOL is the mix loop's; STA is unread; the MOD ones are above
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
