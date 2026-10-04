#ifndef POCKETTRACKER_SONGCORE_SCALES_H
#define POCKETTRACKER_SONGCORE_SCALES_H

// ─── Scale quantization ───────────────────────────────────────────────────────────────────────────
//
// Given a scale, a key and a MIDI note: which notes exist. Pure; every consumer asks these functions,
// so "what is in this scale" has one answer.
// ⚠️ The chromatic case must leave every note exactly where it is (it is every existing song's scale),
// so it is checked explicitly at the top of each entry point rather than falling out of the search.
// Quantizing is by PITCH CLASS; a snap may cross an octave boundary, and that is correct.

#include <algorithm>

#include "effects.h"   // FX_SCA / FX_SCG and their nibble split
#include "model.h"

namespace songcore {

/** True modulo — C++'s % is wrong for negatives. */
inline int scale_mod12(int v) { return ((v % 12) + 12) % 12; }

/** Is this MIDI note in the scale? Degree = its distance above the key, in pitch classes. */
inline bool scale_contains(const Scale& s, int key, int midi) {
    if (midi < 0) return false;
    if (scale_is_chromatic(s)) return true;
    return s.enabled[static_cast<size_t>(scale_mod12(midi - key))] != 0;
}

/**
 * The nearest in-scale note, searching OUTWARD from `midi`, ties broken UPWARD (reads as a leading
 * tone). Returns `midi` unchanged when the scale is chromatic, the note empty (< 0), or the scale
 * has no degree at all (only a hand-edited file can do that).
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
 * Walk `steps` scale degrees from `midi`, clamped to [lo, hi] — the note cursor's A+LEFT/RIGHT (a
 * semitone on chromatic, a minor third on a pentatonic).
 * Walks semitone by semitone counting in-scale notes, so a note typed before the scale changed still
 * moves sensibly. At the ends it clamps to the last IN-SCALE note, never to `lo`/`hi` themselves.
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
 * The twelve enable flags as a bit mask, bit 0 = the root, so a cell can ask "is this typeable"
 * without a Project. A malformed pool answers chromatic — an empty mask would forbid every note.
 */
inline unsigned scale_mask(const Scale& s) {
    if (s.enabled.size() != 12) return 0x0FFFu;
    unsigned m = 0;
    for (int d = 0; d < 12; ++d)
        if (s.enabled[static_cast<size_t>(d)]) m |= (1u << d);
    return m == 0 ? 0x0FFFu : m;
}

/** A pool slot, clamped. Out of range answers slot 00 (the song's base scale), not chromatic. */
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
 * The scale a PHRASE puts one of its rows in: the last `SCA` / `SCG` on or above `row`, else slot 00
 * and the project key. What the note cursor asks.
 * ⚠️ Reads the SONG, never `TrackState`: the scheduler runs two phrases ahead, so its live scale would
 * quantize to a bar not yet heard. The authored cells read the same playing or stopped.
 * ⚠️ Resolved as `Sequencer` does: last-wins per command across the three slots, then SCG, then SCA
 * on top. A command on the cursor's own row counts.
 * ⏸️ Sees this phrase only: an `SCA` earlier in the chain, or an `SCG` on another track, is heard at
 * playback but invisible here. Widening needs the chain the phrase is viewed through.
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

/** How many degrees this scale has, 1..12 — the SCALE screen's LEN, and the guard against turning
 *  off the last one. */
inline int scale_degree_count(const Scale& s) {
    if (s.enabled.size() != 12) return 12;
    int n = 0;
    for (int e : s.enabled) n += (e != 0);
    return n;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SCALES_H
