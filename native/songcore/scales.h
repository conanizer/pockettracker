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

#include "effects.h"   // FX_SCA / FX_SCG and their nibble split — the phrase walk below reads them
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
 * A slot of the project's pool, clamped. Out of range answers slot 00 rather than the chromatic
 * default, because a pool that is short is a malformed file and slot 00 is the scale the song was
 * written against; a hand-typed `SCA` slot beyond the pool is the same case.
 */
inline const Scale& scale_at(const Project& p, int slot) {
    static const Scale kChromatic{};
    if (p.scales.empty()) return kChromatic;
    const int last = static_cast<int>(p.scales.size()) - 1;
    return p.scales[static_cast<size_t>(slot < 0 ? 0 : (slot > last ? 0 : slot))];
}

/** A scale slot together with the key it is positioned at — what a lookup answers with. */
struct ScaleAt {
    int slot = 0;
    int key  = 0;
};

/**
 * The scale a PHRASE puts one of its rows in: the last `SCA` (or `SCG`) on or above `row`, falling
 * back to slot 00 and the project's key when the phrase carries none. This is what the note cursor
 * asks, so that typing under an `SCA` offers that command's notes.
 *
 * ⚠️⚠️ **IT READS THE SONG, NEVER `TrackState`, AND THAT DISTINCTION IS THE WHOLE SAFETY ARGUMENT.**
 * A track's live scale is scheduler state, and the scheduler runs TWO PHRASES AHEAD of what is being
 * heard — wiring the cursor to it would quantize a typed note to the scale of a bar the player has
 * not reached yet, the shape of every LIVE-mode defect this project has had. What this walks instead
 * is the authored cells of the phrase that is on screen: a value the transport cannot move, that
 * reads the same whether anything is playing, and that a screenshot tool can reproduce.
 *
 * ⚠️ The two commands are resolved exactly as `Sequencer` resolves them, or the cursor and the sound
 * would disagree about a step carrying both: within one row the last of each wins across the three
 * slots, then `SCG` is applied and `SCA` second — the narrower command over the broader one.
 *
 * ⚠️ A command on the cursor's OWN row counts, because the scheduler applies both above the note on
 * their step. Typing a note on the same row as an `SCA` gets that command's scale, which is what the
 * screen shows.
 *
 * ⏸️ It sees this phrase and nothing else. An `SCA` in an EARLIER phrase of the same chain, and an
 * `SCG` typed on another track, both reach this row at playback and are invisible here — the cursor
 * then offers slot 00 while the sound follows the command. Widening the walk to the chain is the
 * obvious next step and needs the chain the phrase is being viewed THROUGH, which a phrase reached
 * from the pool does not have.
 */
inline ScaleAt phrase_scale_at_row(const Phrase& ph, int row, int projectKey) {
    ScaleAt at{0, projectKey};
    const int last = std::min(row, static_cast<int>(ph.steps.size()) - 1);
    for (int r = 0; r <= last; ++r) {
        const PhraseStep& step = ph.steps[static_cast<size_t>(r)];
        int sca = -1, scg = -1;
        for (int slot = 1; slot <= 3; ++slot) {
            const int type = step_fx_type(step, slot);
            if (type == FX_SCG)      scg = step_fx_value(step, slot);
            else if (type == FX_SCA) sca = step_fx_value(step, slot);
        }
        if (scg >= 0) { at.slot = scale_cmd_slot(scg); at.key = scale_cmd_key(scg); }
        if (sca >= 0) { at.slot = scale_cmd_slot(sca); at.key = scale_cmd_key(sca); }
    }
    return at;
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
