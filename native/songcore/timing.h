#ifndef POCKETTRACKER_SONGCORE_TIMING_H
#define POCKETTRACKER_SONGCORE_TIMING_H

// Timing, groove and transpose math for the scheduler.
// ⚠️ framesPerStep is binary64 truncated, evaluated `60000.0 / tempo / 4.0 * sr / 1000.0` left to
// right — change the order and every golden changes.

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

// The rulers a groove can be written in, as tics per plain step; four steps make the quarter note.
inline constexpr int GROOVE_RULERS[]     = {6, 12, 24};
inline constexpr int GROOVE_RULER_COUNT  = 3;
inline constexpr int GROOVE_DEFAULT_RULER = 12;

// The groove's ruler, or the default for a value no ruler has (a hand-edited file).
inline int groove_ticks_per_step(const Groove& g) {
    for (int r : GROOVE_RULERS)
        if (g.ticksPerStep == r) return r;
    return GROOVE_DEFAULT_RULER;
}

// Tics for a groove position (wrapping the active window); an all-empty groove gives a plain step.
inline int groove_ticks_for_step(const Groove& g, int groove_step) {
    int len = groove_active_length(g);
    if (len == 0) return groove_ticks_per_step(g);
    return g.steps[groove_step % len];
}

// A groove's steps in another ruler, playing the same; halving rounds. ⚠️ A step never rounds to 0
// (that skips its row). False, unchanged, when a step would pass 255.
inline bool groove_set_ruler(Groove& g, int ticks_per_step) {
    const int from = groove_ticks_per_step(g);
    std::vector<int> out = g.steps;
    for (int& v : out) {
        if (v <= 0) continue;   // the end marker and a skipped row mean the same in every ruler
        v = (v * ticks_per_step * 2 + from) / (from * 2);
        if (v < 1) v = 1;
        if (v > 255) return false;
    }
    g.steps        = out;
    g.ticksPerStep = ticks_per_step;
    return true;
}

// A step's length in frames: scaled by an active groove's tics (0 skips the row).
// ⚠️ MULTIPLY THEN DIVIDE: the scheduler accumulates these, and dividing first drifts a grooved track
// against an ungrooved one. (`frames_per_tic` is an effect's unit inside a step, not this.)
inline int64_t groove_step_duration(const Groove& g, int groove_step,
                                    int64_t frames_per_step_value) {
    if (groove_active_length(g) == 0) return frames_per_step_value;
    return frames_per_step_value * groove_ticks_for_step(g, groove_step) / groove_ticks_per_step(g);
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_TIMING_H
