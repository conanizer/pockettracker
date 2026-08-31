#ifndef POCKETTRACKER_SONGCORE_SCALES_H
#define POCKETTRACKER_SONGCORE_SCALES_H

// ─── Scale quantization ───────────────────────────────────────────────────────────────────────────
//
// The pure half of roadmap 1.A: given a scale, a key and a MIDI note, which notes exist. No screen,
// no engine, no project — every consumer (the note cursor today, the transposes and ARP/PIT later)
// asks the same three functions so that "what is in this scale" has exactly one answer.
//
// ⚠️ THE IDENTITY CASE IS LOAD-BEARING, NOT AN OPTIMISATION. A chromatic scale — the default, and
// what every existing song has — must leave every note exactly where it is, or the feature rewrites
// music nobody asked it to touch. It is checked once, at the top of each entry point, rather than
// falling out of the search: a search that happens to be the identity today is a search that stops
// being the identity the first time someone changes a tie-break.
//
// ⚠️ A note is quantized in PITCH CLASS, so the octave a snap lands in is whatever the semitone
// arithmetic gives. Snapping C#-4 down to C-4 stays in octave 4; snapping B#-style edge cases across
// an octave boundary is the correct answer, not a wrap bug.

#include <algorithm>

#include "model.h"

namespace songcore {

/** True modulo — C++'s % is not it for negatives, and every caller here can hand one in. */
inline int scale_mod12(int v) { return ((v % 12) + 12) % 12; }

/** Is this MIDI note in the scale? Degree = its distance above the key, in pitch classes. */
inline bool scale_contains(const Scale& s, int key, int midi) {
    if (midi < 0) return false;
    if (scale_is_chromatic(s)) return true;
    return s.enabled[static_cast<size_t>(scale_mod12(midi - key))] != 0;
}

/**
 * The nearest note in the scale, searching OUTWARD from `midi` and breaking a tie UPWARD.
 *
 * ⚠️ Upward on a tie is a decision, not a coin toss: a snap must never move a note below where the
 * author put it more often than above, and an equidistant pair (a note exactly between two degrees,
 * which only a whole-tone-ish scale produces) reads better resolved as a leading tone.
 *
 * Returns `midi` unchanged when the scale is chromatic, when the note is empty (< 0), or when the
 * scale has no enabled degree at all — the last is not reachable through the editor (it refuses to
 * disable the twelfth degree) but a hand-edited file can carry it, and silently returning nothing
 * would be a note that cannot be typed.
 */
inline int scale_snap(const Scale& s, int key, int midi) {
    if (midi < 0 || scale_is_chromatic(s)) return midi;
    for (int d = 0; d <= 6; ++d) {
        if (midi + d <= 127 && scale_contains(s, key, midi + d)) return midi + d;
        if (d != 0 && midi - d >= 0 && scale_contains(s, key, midi - d)) return midi - d;
    }
    return midi;  // no degree enabled — a file the editor cannot produce
}

/**
 * Walk `steps` degrees of the scale from `midi`, clamped to [lo, hi].
 *
 * The note cursor's A+LEFT / A+RIGHT: one press is one note OF THE SCALE, which on a pentatonic is a
 * minor third and on the chromatic default is a semitone — so the gesture keeps the meaning it has
 * today and gains a new one only where the author asked for it.
 *
 * ⚠️ It walks semitone by semitone and counts the ones that land, rather than indexing a list of
 * degrees. Indexing needs the note to already BE in the scale; a note typed before the scale changed
 * is not, and the first press must move it somewhere sensible rather than nowhere. Walking answers
 * both cases with one rule.
 *
 * ⚠️ At the ends it CLAMPS to the last in-scale note, not to `lo`/`hi` themselves — the MIDI ceiling
 * is not a scale degree, and stepping up into a note the scale forbids would undo the whole point.
 */
inline int scale_step(const Scale& s, int key, int midi, int steps, int lo, int hi) {
    if (steps == 0) return midi;
    if (scale_is_chromatic(s)) return std::min(hi, std::max(lo, midi + steps));

    const int dir  = steps > 0 ? 1 : -1;
    int       cur  = midi;
    int       left = steps > 0 ? steps : -steps;
    while (left > 0) {
        int probe = cur + dir;
        while (probe >= lo && probe <= hi && !scale_contains(s, key, probe)) probe += dir;
        if (probe < lo || probe > hi) break;  // no further in-scale note: stop on the last one
        cur = probe;
        --left;
    }
    return cur;
}

/**
 * The twelve enable flags as a bit mask, bit 0 = the root. The form the UI's cursor carries, so that
 * a cell can be asked "is this note typeable" without holding a Project.
 *
 * A malformed pool answers "chromatic" rather than "nothing": an empty mask is a cell in which no
 * note can be typed at all.
 */
inline unsigned scale_mask(const Scale& s) {
    if (s.enabled.size() != 12) return 0x0FFFu;
    unsigned m = 0;
    for (int d = 0; d < 12; ++d)
        if (s.enabled[static_cast<size_t>(d)]) m |= (1u << d);
    return m == 0 ? 0x0FFFu : m;
}

/**
 * The scale a track is currently in. ⏸️ TODAY IT IS ALWAYS SLOT 00 — the manual's "scale 00 is the
 * default scale for all 8 tracks" — because the two commands that move a track off it, `SCA` and
 * `SCG`, are the next step. It is a function rather than a literal so that when they land, every
 * caller changes at once instead of one at a time.
 */
inline const Scale& track_scale(const Project& p, int /*trackId*/) {
    static const Scale kChromatic{};
    return p.scales.empty() ? kChromatic : p.scales[0];
}

/**
 * How many degrees this scale actually has, 1..12. The SCALE screen's LEN readout, and the guard the
 * editor uses to refuse turning the last degree off.
 */
inline int scale_degree_count(const Scale& s) {
    if (s.enabled.size() != 12) return 12;
    int n = 0;
    for (int e : s.enabled) n += (e != 0);
    return n;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SCALES_H
