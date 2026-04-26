#pragma once
#include <cmath>
#include "../../mod-system.h"
#include "../primitives/lfo-oscillator.h"

// tickLFO — advance one audio block for an LFO mod slot (type 3).
//   sr    : sample rate in Hz (for Hz → radians/frame conversion).
//   rMult : rate multiplier from mod-to-mod routing (1.0 = no mod).
// Updates mod.lfoPhase and mod.envValue in place.
//
// SCALAR (type=6): constant output equal to mod.amount — not modelled here.
// It is a degenerate LFO that never oscillates. It is handled as its own short
// branch in mod-runner.h. Whether type=6 should instead be a dedicated
// ModSourceId (static per-note scalar, like MOD_SRC_VELOCITY) is unresolved;
// left as-is until the feature is actually used. — TODO: revisit before shipping.
inline void tickLFO(VoiceModSlot& mod, int numFrames, float sr, float rMult) {
    float effHz       = mod.lfoHz * rMult;
    float phaseAdvance = 2.0f * (float)M_PI * effHz / sr * numFrames;
    mod.lfoPhase += phaseAdvance;
    while (mod.lfoPhase >= 2.0f * (float)M_PI) mod.lfoPhase -= 2.0f * (float)M_PI;
    mod.envValue = lfoShape(mod.lfoPhase, mod.oscShape);
}
