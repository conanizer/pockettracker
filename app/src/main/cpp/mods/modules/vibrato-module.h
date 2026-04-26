#pragma once
#include <cmath>
#include "../../mod-system.h"
#include "../../sampler-voice.h"
#include "../primitives/lfo-oscillator.h"

// tickVibrato — advance vibrato LFO state (PVB / PVX effects) by one block.
// Updates voice.vibratoPhase in place; writes MOD_SRC_VIBRATO into modSourceValues[].
// Uses lfoShape() so all oscillator shapes are available (currently shape=1/SIN only,
// but the primitive is now in place if shape selection is added later).
// Must be called before runModMatrix so the source value is ready when routes are processed.
inline void tickVibrato(Voice& voice, int numFrames, float sr) {
    if (voice.vibratoActive) {
        float phaseIncrement = (2.0f * (float)M_PI * voice.vibratoSpeed / sr) * numFrames;
        voice.vibratoPhase += phaseIncrement;
        while (voice.vibratoPhase >= 2.0f * (float)M_PI) {
            voice.vibratoPhase -= 2.0f * (float)M_PI;
        }
    }
    voice.modSourceValues[MOD_SRC_VIBRATO] = voice.vibratoActive
        ? lfoShape(voice.vibratoPhase, 1) * voice.vibratoDepth
        : 0.0f;
}
