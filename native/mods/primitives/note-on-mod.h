#pragma once
#include <cmath>
#include <cstdint>
#include "lfo-oscillator.h"
#include "rng.h"
#include "songcore/voice_codes.h"

// A mod slot at a note's first frame.

// RETG and ONCE start the cycle at the note; FREE and HOLD follow the song's clock.
inline float lfo_phase_at_note_on(const songcore::EngineModSlot& m, int64_t frame, float sampleRate) {
    if (m.lfoTrigMode != songcore::LFO_FREE && m.lfoTrigMode != songcore::LFO_HOLD) return 0.0f;
    const double cycles = (double)frame * (double)m.lfoHz / (double)sampleRate;
    return (float)(fmod(cycles, 1.0) * 2.0 * M_PI);
}

// The RND/DRNK seed of slot `slot` for a note at `frame`. Never 0.
inline uint32_t lfo_seed_at_note_on(int64_t frame, int slot, uint32_t entropy) {
    return (((uint32_t)(uint64_t)frame) * 747796405u ^ (uint32_t)(slot + 1) * 2891336453u ^ entropy) | 1u;
}

// An LFO's shape value as it leaves the slot: as is, moved up into 0..1, or down into −1..0.
inline float lfo_polarity_out(int polarity, float v) {
    if (polarity == songcore::LFO_UP)   return 0.5f * v + 0.5f;
    if (polarity == songcore::LFO_DOWN) return -(0.5f * v + 0.5f);
    return v;
}
