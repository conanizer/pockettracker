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
// double rounding, and therefore the truncated frame count, is bit-identical to the JVM.
// tools/ptresolve proves that against a JVM-emitted golden.
//
// ⚠️ ONE FUNCTION HERE DELIBERATELY NO LONGER MATCHES THE JVM: `groove_step_duration`, which
// multiplies before dividing where Kotlin divided first. Kotlin's order lost up to eleven frames on
// every grooved step and the scheduler accumulates them, so grooved playback ran fast. The golden
// still records Kotlin's answer and ptresolve derives the exact departure from it rather than
// adopting the new number — see that function, and `tools/testdata/README.md` §1.

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

// Chain per-row transpose (Chain.getTransposeSemitones(index)). Out-of-range rows transpose by
// nothing: `transposeValues` is the same shape as `phraseRefs` and is parsed the same way, so it can
// be shorter than CHAIN_ROWS in a hand-edited or half-written file. See `chain_phrase_ref`.
inline int chain_transpose_semitones(const Chain& chain, int index) {
    if (index < 0 || index >= static_cast<int>(chain.transposeValues.size())) return 0;
    return byte_to_signed_semitones(chain.transposeValues[static_cast<size_t>(index)]);
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

// Composed per-step duration in frames. An active groove scales the step by its tic count — which can
// be 0, meaning skip the row; no active groove is exactly one plain step.
//
// ⚠️⚠️ MULTIPLY THEN DIVIDE. `framesPerTic × tics` divides first and throws away up to
// TICS_PER_STEP−1 frames on every step, and the scheduler ACCUMULATES these durations, so the loss
// compounds: a full-length 12-tic step came out SHORTER than a plain step, and a grooved track ran
// measurably fast against an ungrooved one on the same song — at 140 BPM they separated by a whole
// step inside 33 bars. Multiplying first makes a full-length step exactly `frames_per_step_value` and
// leaves under one frame of residual.
//
// ⚠️ This is a DELIBERATE DEPARTURE FROM THE JVM, which truncated. It is the one thing in this file
// that no longer reproduces `PlaybackController.schedulePhrase`, and `tools/ptresolve` states the
// departure as an exact offset off the recorded golden rather than adopting the new answer.
//
// ⚠️ `frames_per_tic` is still the unit an EFFECT positions itself in WITHIN a step (see the
// scheduler's `stepDuration / TICS_PER_STEP`). It is simply not what a step's LENGTH is built from,
// which is why it is no longer a parameter here.
inline int64_t groove_step_duration(const Groove& g, int groove_step,
                                    int64_t frames_per_step_value) {
    if (groove_active_length(g) == 0) return frames_per_step_value;
    return frames_per_step_value * groove_ticks_for_step(g, groove_step) / TICS_PER_STEP;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_TIMING_H
