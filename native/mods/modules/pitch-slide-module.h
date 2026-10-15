#pragma once
#include <cmath>
#include "../mod-system.h"
#include "../../sampler-voice.h"

// PSL / PBN, one block, for both voice types (same field names). At the target the slide stops
// and the pitch holds: a PBN stays bent, a PSL lands on 0.
template <typename V>
inline void advancePitchSlide(V& voice, int numFrames) {
    if (!voice.pitchSliding) return;
    float delta      = voice.pitchSlideTarget - voice.pitchOffset;
    float totalDelta = voice.pitchSlideRate * numFrames;
    if (fabsf(totalDelta) >= fabsf(delta)) {
        voice.pitchOffset  = voice.pitchSlideTarget;
        voice.pitchSliding = false;
    } else {
        voice.pitchOffset += totalDelta;
    }
}

// The sampler's: the advance plus its mod source. Before runModMatrix.
inline void tickPitchSlide(Voice& voice, int numFrames) {
    advancePitchSlide(voice, numFrames);
    voice.modSourceValues[MOD_SRC_PITCH_SLIDE] = voice.pitchOffset;
}
