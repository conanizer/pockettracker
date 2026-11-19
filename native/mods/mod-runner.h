#pragma once
#include <cstring>
#include <cmath>
#include "mod-system.h"
#include "modules/ahd-module.h"
#include "modules/adsr-module.h"
#include "modules/lfo-module.h"

// An envelope slot's curved factors for steps of `numFrames`, at its mod-to-mod rate. ENV always
// falls FAST; a straight curve has none.
inline EnvFactors env_factors_for(const VoiceModSlot& mod, int numFrames) {
    if (mod.type != ModType::ENV && mod.curve == songcore::CURVE_LINEAR) return {};
    const bool analog = mod.type != ModType::ENV && mod.curve == songcore::CURVE_ANALOG;
    return env_factors(mod, static_cast<float>(numFrames) * fmaxf(0.01f, mod.effectiveRateMult), analog);
}

// One step of `numFrames` of an envelope slot; `f` is env_factors_for() the same step.
inline void stepEnvelope(VoiceModSlot& mod, int numFrames, const EnvFactors& f) {
    const float rMult  = fmaxf(0.01f, mod.effectiveRateMult);
    const bool  linear = mod.curve == songcore::CURVE_LINEAR;
    const bool  analog = mod.curve == songcore::CURVE_ANALOG;
    switch (mod.type) {
        case ModType::AHD:
            if (linear) tickAHD(mod, numFrames, rMult);
            else        tickAHDCurved(mod, f, numFrames, rMult, analog);
            break;
        case ModType::ADSR:
            if (linear) tickADSR(mod, numFrames, rMult);
            else        tickENV(mod, f, analog);
            break;
        case ModType::ENV:
            tickENV(mod, f, false);
            break;
        default:
            break;
    }
}

// A VOL envelope the mix loop steps every sample itself, so its stages start on their own frame.
inline bool vol_envelope_at_sample(const VoiceModSlot& mod) {
    return mod.dest == ModDest::VOLUME && mod_is_envelope(mod.type);
}

// One voice's modulation for one block. ⚠️ Runs after tickPitchSlide and tickVibrato, which write
// their sources first. With `volAtSample` the VOL envelopes are left to the mix loop.
inline void runModMatrix(IAudioVoice& voice, int numFrames, float sr, bool volAtSample = false) {
    using namespace songcore;
    // LFO and SCALAR slots feed the LFO sources, envelopes and TRK the ENV ones.
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

    // Mod-to-mod: a slot aimed at another's AMT adds to its amount, at its RATE multiplies its rate
    // by 1 + source × AMT — ×1 at AMT 00 or a source at rest, up to ×2, and an LFO's lower half slows
    // it (to a twentieth at FF).
    {
        float amtOffset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float rateMult[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
        for (int m = 0; m < 4; m++) {
            const VoiceModSlot& src = voice.voiceMods[m];
            if (!mod_running(src)) continue;
            const songcore::ModTarget to = songcore::mod_target(src.dest, m);
            if (to.slot < 0) continue;
            const bool toAmt  = to.part != songcore::ModTargetPart::RATE;
            const bool toRate = to.part != songcore::ModTargetPart::AMT;
            const int target = to.slot;
            // A UNI- LFO (0..−1) takes amount away and slows the rate down to a twentieth.
            const bool lfo  = src.type == ModType::LFO;
            const bool down = lfo && src.lfoPolarity == LFO_DOWN;
            float norm = (lfo && src.lfoPolarity == LFO_BI) ? (src.envValue * 0.5f + 0.5f)
                       : down ? src.envValue : fmaxf(0.0f, src.envValue);
            if (toAmt) amtOffset[target] += norm * src.amount;
            if (toRate) {
                const float swing = lfo ? src.envValue : fmaxf(0.0f, src.envValue);
                rateMult[target] *= fmaxf(0.05f, 1.0f + swing * src.amount);
            }
        }
        for (int m = 0; m < 4; m++) {
            voice.voiceMods[m].effectiveAmt      = fmaxf(0.0f, fminf(1.0f, voice.voiceMods[m].amount + amtOffset[m]));
            voice.voiceMods[m].effectiveRateMult = rateMult[m];
        }
    }

    for (int m = 0; m < 4; m++) {
        VoiceModSlot& mod = voice.voiceMods[m];
        if (!mod_running(mod)) continue;

        const float rMult = fmaxf(0.01f, mod.effectiveRateMult);

        switch (mod.type) {
            case ModType::AHD:
            case ModType::ADSR:
            case ModType::ENV:
                if (!(volAtSample && vol_envelope_at_sample(mod)))
                    stepEnvelope(mod, numFrames, env_factors_for(mod, numFrames));
                break;
            case ModType::LFO:
                tickLFO(mod, numFrames, sr, rMult);
                break;
            case ModType::SCALAR:   // a constant; its AMT is applied where it lands, once
                mod.envValue = 1.0f;
                break;
            case ModType::TRACKING: {
                // The note's octaves from C-4. At AMT FF the cutoff follows the note an octave an octave
                // (CUT spans log2(1000) octaves over its 255 units); any other setting moves a quarter of
                // its full swing an octave. Constant for the note, so it does not ramp in.
                const float perOctave = mod.dest == ModDest::FILTER_CUTOFF ? 1.0f / 9.9657843f : 0.25f;
                mod.envValue     = voice.modSourceValues[MOD_SRC_KEYTRACK] * perOctave;
                mod.prevEnvValue = mod.envValue;
                break;
            }
            case ModType::VELOCITY:   // constant for the note, like TRK
                mod.envValue     = voice.modSourceValues[MOD_SRC_VELOCITY];
                mod.prevEnvValue = mod.envValue;
                break;
            default:
                break;
        }

        voice.modSourceValues[sourceOf(mod, m)] = mod.envValue;
    }

    // Up to 4 slot routes, rebuilt every block (mod-to-mod moves the amounts), plus 4 fixed ones.
    ModRoute routes[4 + 5];   // the slots, then the five fixed routes below
    int routeCount = 0;

    for (int m = 0; m < 4; m++) {
        const VoiceModSlot& mod = voice.voiceMods[m];
        if (mod.type == ModType::NONE) continue;

        const ModSourceId srcId = sourceOf(mod, m);
        ParamId destId;
        float   scale;
        switch (mod.dest) {
            case ModDest::PAN:           destId = PARAM_PAN;         scale = mod.effectiveAmt * 0.5f;    break;
            case ModDest::PITCH:         destId = PARAM_PITCH;       scale = mod.effectiveAmt * 12.0f;   break;
            case ModDest::FINE_PITCH:    destId = PARAM_PITCH;       scale = mod.effectiveAmt * 1.0f;    break;
            case ModDest::FILTER_CUTOFF: destId = PARAM_FILTER_CUT;  scale = mod.effectiveAmt * 255.0f;  break;
            case ModDest::FILTER_RES:    destId = PARAM_FILTER_RES;  scale = mod.effectiveAmt * 255.0f;  break;
            // A MACRO voice's two controls; read by no other voice.
            case ModDest::TIMBRE:        destId = PARAM_MACRO_TIMBRE; scale = mod.effectiveAmt * 255.0f; break;
            case ModDest::COLOR:         destId = PARAM_MACRO_COLOR;  scale = mod.effectiveAmt * 255.0f; break;
            default: continue;   // VOL is the mix loop's; STA is unread; the MOD ones are above
        }
        routes[routeCount++] = { srcId, destId, scale, MOD_SRC_NONE, 0.0f };
    }

    // The fixed routes. Volume = table × the velocity (the depth); the instrument's VOL apart, since
    // the sends are tapped between the two.
    routes[routeCount++] = { MOD_SRC_TABLE_PITCH, PARAM_PITCH,     1.0f, MOD_SRC_NONE, 0.0f };
    routes[routeCount++] = { MOD_SRC_PITCH_SLIDE, PARAM_PITCH,     1.0f, MOD_SRC_NONE, 0.0f };
    routes[routeCount++] = { MOD_SRC_VIBRATO,     PARAM_PITCH,     1.0f, MOD_SRC_NONE, 0.0f };
    routes[routeCount++] = { MOD_SRC_TABLE_VOL,   PARAM_VOL,       voice.params.base[PARAM_VOL],
                              MOD_SRC_NONE, 0.0f };
    routes[routeCount++] = { MOD_SRC_PHRASE_VOL,  PARAM_INSTR_VOL, 1.0f, MOD_SRC_NONE, 0.0f };

    processRoutes(voice.modSourceValues, voice.modDestValues, routes, routeCount);

    // The mix loop reads params.get().
    for (int p = 0; p < PARAM_COUNT; p++) {
        voice.params.mod[p] = voice.modDestValues[p];
    }
}
