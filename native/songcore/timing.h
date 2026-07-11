#ifndef POCKETTRACKER_SONGCORE_TIMING_H
#define POCKETTRACKER_SONGCORE_TIMING_H

// ─── Timing, groove, and transpose math ───────────────────────────────────────────────────────────
//
// Pure functions ported 1:1 from the Kotlin sequencer's timing layer:
//   * TICS_PER_STEP + framesPerStep()               — core/data/TrackerData.kt
//   * byteToSignedSemitones() + transpose readers    — core/data/TrackerData.kt
//   * Groove.activeLength() / getTicksForStep()      — core/data/TrackerData.kt
//   * the per-step groove duration composition       — PlaybackController.schedulePhrase()
//
// These are the stateless arithmetic the scheduler (songcore S4) calls; NO per-track state lives
// here. TrackerData.kt / PlaybackController.kt are the executable spec; this header is its C++ twin.
//
// framesPerStep is a binary64 (Double) computation truncated to Long. The evaluation ORDER is
// replicated verbatim — 60000.0 / tempo / 4.0 * sr / 1000.0, left-to-right, same precedence — so the
// double rounding, and therefore the truncated frame count, is bit-identical to the JVM. Likewise the
// groove path multiplies the already-truncated framesPerTic (never re-derives from framesPerStep), so
// a 12-tic groove step does NOT equal a plain step — that rounding drift is intentional and preserved.
// tools/ptresolve proves all of this against a JVM-emitted golden.

#include <cstdint>
#include "model.h"

namespace songcore {

// Tics per phrase step. Mirrors TrackerData.TICS_PER_STEP — do not hardcode 12 anywhere else.
constexpr int TICS_PER_STEP = 12;

// Frames per phrase step (one 16th note) at `tempo` BPM and `sample_rate` Hz:
// msPerStep = 60000 / tempo / 4, then × sampleRate / 1000. The evaluation order and the truncating
// cast mirror Kotlin's `(60000.0 / tempo / 4.0 * sampleRate / 1000.0).toLong()` exactly.
inline int64_t frames_per_step(int tempo, int sample_rate) {
    return static_cast<int64_t>(60000.0 / tempo / 4.0 * sample_rate / 1000.0);
}

// Frames per tic — the scheduler's `framesPerStep / TICS_PER_STEP` (Long integer division).
inline int64_t frames_per_tic(int64_t frames_per_step_value) {
    return frames_per_step_value / TICS_PER_STEP;
}

// Two's-complement decode of a 0x00–0xFF transpose byte to signed semitones:
// 0x00 = 0, 0x01–0x7F = +1..+127, 0x80–0xFF = −128..−1. Mirrors byteToSignedSemitones().
inline int byte_to_signed_semitones(int b) {
    int v = b & 0xFF;
    return v < 0x80 ? v : v - 256;
}

// Chain per-row transpose (Chain.getTransposeSemitones(index)); index assumed 0..15, as the callers
// guarantee.
inline int chain_transpose_semitones(const Chain& chain, int index) {
    return byte_to_signed_semitones(chain.transposeValues[index]);
}

// Project-wide transpose (Project.getTransposeSemitones()).
inline int project_transpose_semitones(const Project& project) {
    return byte_to_signed_semitones(project.transpose);
}

// ─── Groove ─────────────────────────────────────────────────────────────────────────────────────
// Kept as free functions so model.h stays a pure data mirror; all behaviour lives in the logic layer.

// Number of active groove steps — those before the first -1 end marker. Mirrors Groove.activeLength().
inline int groove_active_length(const Groove& g) {
    for (size_t i = 0; i < g.steps.size(); ++i)
        if (g.steps[i] == -1) return static_cast<int>(i);
    return static_cast<int>(g.steps.size());
}

// Tic duration for a groove position (wraps around the active window). Mirrors
// Groove.getTicksForStep(grooveStep); an all-empty groove falls back to standard step timing.
inline int groove_ticks_for_step(const Groove& g, int groove_step) {
    int len = groove_active_length(g);
    if (len == 0) return TICS_PER_STEP;
    return g.steps[groove_step % len];
}

// Composed per-step duration in frames, exactly as PlaybackController.schedulePhrase computes it:
// an active groove (activeLength > 0) gives `framesPerTic × ticksForStep(pos)` — which can be 0 (skip
// the row) and which does NOT equal framesPerStep for a 12-tic step because framesPerTic already
// truncated; no active groove falls back to the exact framesPerStep to avoid that drift.
inline int64_t groove_step_duration(const Groove& g, int groove_step,
                                    int64_t frames_per_step_value, int64_t frames_per_tic_value) {
    if (groove_active_length(g) > 0)
        return frames_per_tic_value * groove_ticks_for_step(g, groove_step);
    return frames_per_step_value;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_TIMING_H
