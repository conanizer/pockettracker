#pragma once
#include <cmath>
#include "../mod-system.h"
#include "../../sampler-voice.h"
#include "../primitives/lfo-oscillator.h"

// advanceVibratoPhase — the ONE vibrato LFO phase advance (PVB / PVX), shared by the
// sampler path (tickVibrato below) and SoundfontVoice::applyPitchMod. Duck-typed template
// over the identical vibratoActive/vibratoSpeed/vibratoPhase fields on both voice types.
template <typename V>
inline void advanceVibratoPhase(V& voice, int numFrames, float sr) {
    if (!voice.vibratoActive) return;
    float phaseIncrement = (2.0f * (float)M_PI * voice.vibratoSpeed / sr) * numFrames;
    voice.vibratoPhase += phaseIncrement;
    while (voice.vibratoPhase >= 2.0f * (float)M_PI) {
        voice.vibratoPhase -= 2.0f * (float)M_PI;
    }
}

// tickVibrato — sampler-path block advance: shared phase advance + write MOD_SRC_VIBRATO
// into modSourceValues[]. Uses lfoShape() so all oscillator shapes are available (currently
// shape=1/SIN only, but the primitive is in place if shape selection is added later).
// Must be called before runModMatrix so the source value is ready when routes are processed.
inline void tickVibrato(Voice& voice, int numFrames, float sr) {
    advanceVibratoPhase(voice, numFrames, sr);
    voice.modSourceValues[MOD_SRC_VIBRATO] = voice.vibratoActive
        ? lfoShape(voice.vibratoPhase, 1) * voice.vibratoDepth
        : 0.0f;
}
