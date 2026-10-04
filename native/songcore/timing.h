#ifndef POCKETTRACKER_SONGCORE_TIMING_H
#define POCKETTRACKER_SONGCORE_TIMING_H

// ─── Timing, groove, and transpose math ───────────────────────────────────────────────────────────
//
// The stateless arithmetic the scheduler calls; no per-track state here.
// framesPerStep is binary64 truncated to an integer, in a fixed evaluation order
// (60000.0 / tempo / 4.0 * sr / 1000.0, left to right) — ⚠️ change the order and the frame counts,
// and every golden, change. A test checks it against a recorded golden.

#include <cstdint>
#include "model.h"
#include "program.h"   // TICS_PER_STEP — the note derivation needs it lower down

namespace songcore {

// Frames per phrase step (a 16th) at `tempo` BPM and `sample_rate` Hz. ⚠️ Keep the evaluation order
// and the truncating cast exactly.
inline int64_t frames_per_step(int tempo, int sample_rate) {
    return static_cast<int64_t>(60000.0 / tempo / 4.0 * sample_rate / 1000.0);
}

// Frames per tic — `framesPerStep / TICS_PER_STEP`, integer division.
inline int64_t frames_per_tic(int64_t frames_per_step_value) {
    return frames_per_step_value / TICS_PER_STEP;
}

// Two's-complement decode of a 0x00–0xFF transpose byte: 0x01–0x7F = +1..+127, 0x80–0xFF = −128..−1.
inline int byte_to_signed_semitones(int b) {
    int v = b & 0xFF;
    return v < 0x80 ? v : v - 256;
}

// Chain per-row transpose. Out-of-range rows transpose by nothing: `transposeValues` can be shorter
// than CHAIN_ROWS in a hand-edited file (see `chain_phrase_ref`).
inline int chain_transpose_semitones(const Chain& chain, int index) {
    if (index < 0 || index >= static_cast<int>(chain.transposeValues.size())) return 0;
    return byte_to_signed_semitones(chain.transposeValues[static_cast<size_t>(index)]);
}

// Project-wide transpose.
inline int project_transpose_semitones(const Project& project) {
    return byte_to_signed_semitones(project.transpose);
}

// ─── Groove ─────────────────────────────────────────────────────────────────────────────────────
// Free functions, so model.h stays pure data.

// Number of active groove steps — those before the first -1 end marker.
inline int groove_active_length(const Groove& g) {
    for (size_t i = 0; i < g.steps.size(); ++i)
        if (g.steps[i] == -1) return static_cast<int>(i);
    return static_cast<int>(g.steps.size());
}

// Tics for a groove position (wrapping the active window); an all-empty groove gives a plain step.
inline int groove_ticks_for_step(const Groove& g, int groove_step) {
    int len = groove_active_length(g);
    if (len == 0) return TICS_PER_STEP;
    return g.steps[groove_step % len];
}

// Per-step duration in frames. An active groove scales the step by its tic count — 0 skips the row;
// no active groove is one plain step.
//
// ⚠️ MULTIPLY THEN DIVIDE. The scheduler ACCUMULATES these durations; dividing first loses up to
// TICS_PER_STEP−1 frames per step, and a grooved track drifts fast against an ungrooved one (a whole
// step in 33 bars at 140 BPM). This deliberately differs from the recorded golden, which the test
// accounts for as an exact offset.
// `frames_per_tic` is still the unit an EFFECT positions itself in within a step — just not what a
// step's length is built from.
inline int64_t groove_step_duration(const Groove& g, int groove_step,
                                    int64_t frames_per_step_value) {
    if (groove_active_length(g) == 0) return frames_per_step_value;
    return frames_per_step_value * groove_ticks_for_step(g, groove_step) / TICS_PER_STEP;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_TIMING_H
