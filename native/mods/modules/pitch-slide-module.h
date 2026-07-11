#pragma once
#include <cmath>
#include "../mod-system.h"
#include "../../sampler-voice.h"

// advancePitchSlide — the ONE pitch-slide/bend state advance (PSL / PBN), shared by the
// sampler path (tickPitchSlide below) and SoundfontVoice::applyPitchMod. Duck-typed template:
// both voice types carry identical pitchOffset/pitchSlideTarget/pitchSlideRate/pitchSliding
// fields; resolving at compile time keeps the audio thread free of dispatch.
// At the target the slide stops (pitchSliding=false) and pitchOffset HOLDS — a stopped PBN
// keeps its bent pitch; only a PSL (target 0) lands back at unity.
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

// tickPitchSlide — sampler-path block advance: shared state advance + write
// MOD_SRC_PITCH_SLIDE into modSourceValues[] so processRoutes picks it up.
// Must be called before runModMatrix so the source value is ready when routes are processed.
inline void tickPitchSlide(Voice& voice, int numFrames) {
    advancePitchSlide(voice, numFrames);
    voice.modSourceValues[MOD_SRC_PITCH_SLIDE] = voice.pitchOffset;
}
