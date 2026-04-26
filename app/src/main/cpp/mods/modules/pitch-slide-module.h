#pragma once
#include <cmath>
#include "../../mod-system.h"
#include "../../sampler-voice.h"

// tickPitchSlide — advance pitch slide / bend state (PSL / PBN effects) by one block.
// Updates voice.pitchOffset and voice.pitchSliding in place.
// Writes MOD_SRC_PITCH_SLIDE into modSourceValues[] so processRoutes picks it up.
// Must be called before runModMatrix so the source value is ready when routes are processed.
inline void tickPitchSlide(Voice& voice, int numFrames) {
    if (voice.pitchSliding) {
        float delta      = voice.pitchSlideTarget - voice.pitchOffset;
        float totalDelta = voice.pitchSlideRate * numFrames;
        if (fabsf(totalDelta) >= fabsf(delta)) {
            voice.pitchOffset = voice.pitchSlideTarget;
            if (fabsf(voice.pitchSlideTarget) < 100.0f) {
                voice.pitchSliding = false;
            }
        } else {
            voice.pitchOffset += totalDelta;
        }
    }
    voice.modSourceValues[MOD_SRC_PITCH_SLIDE] = voice.pitchOffset;
}
