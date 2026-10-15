#pragma once
#include <cmath>
#include "../mod-system.h"
#include "../../sampler-voice.h"
#include "../primitives/lfo-oscillator.h"

// PVB / PVX's phase, one block, for both voice types (same field names).
template <typename V>
inline void advanceVibratoPhase(V& voice, int numFrames, float sr) {
    if (!voice.vibratoActive) return;
    float phaseIncrement = (2.0f * (float)M_PI * voice.vibratoSpeed / sr) * numFrames;
    voice.vibratoPhase += phaseIncrement;
    while (voice.vibratoPhase >= 2.0f * (float)M_PI) {
        voice.vibratoPhase -= 2.0f * (float)M_PI;
    }
}

// The sampler's: the advance plus its mod source (a sine). Before runModMatrix.
inline void tickVibrato(Voice& voice, int numFrames, float sr) {
    advanceVibratoPhase(voice, numFrames, sr);
    voice.modSourceValues[MOD_SRC_VIBRATO] = voice.vibratoActive
        ? lfoShape(voice.vibratoPhase, 1) * voice.vibratoDepth
        : 0.0f;
}
