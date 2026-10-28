#ifndef POCKETTRACKER_SONGCORE_SCALES_H
#define POCKETTRACKER_SONGCORE_SCALES_H

// Scale quantization: which notes a scale and key allow. Every consumer asks here.
// ⚠️ Chromatic must leave every note where it is, so each entry point checks it first.
// By PITCH CLASS — a snap may cross an octave.

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

// The nearest in-scale note, ties UPWARD. `midi` unchanged when chromatic, empty (< 0) or degree-less.
inline int scale_snap(const Scale& s, int key, int midi) {
    if (midi < 0 || scale_is_chromatic(s)) return midi;
    for (int d = 0; d <= 6; ++d) {
        if (midi + d <= 127 && scale_contains(s, key, midi + d)) return midi + d;
        if (d != 0 && midi - d >= 0 && scale_contains(s, key, midi - d)) return midi - d;
    }
    return midi;  // no degree enabled — a file the editor cannot produce
}


// The twelve flags as a mask, bit 0 = the root. Malformed answers chromatic.
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

// The scale a phrase row is in, for the note cursor: the last SCA / SCG on or above `row`, resolved as
// the Sequencer does; else slot 00 and the project key.
// ⚠️ Reads the cells, never `TrackState` — the scheduler runs ahead of what is heard.
// Sees this phrase only: an earlier SCA in the chain, or another track's SCG, is not seen here.
inline ScaleAt phrase_scale_at_row(const Phrase& ph, int row, int projectKey) {
    ScaleAt at{0, projectKey};
    const int last = std::min(row, static_cast<int>(ph.steps.size()) - 1);
    for (int r = 0; r <= last; ++r) {
        const PhraseStep step = as_played(ph.steps[static_cast<size_t>(r)]);
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

// The scale a note on `row` is held to with `instrument`; null where `apply_track_scale` exempts it.
struct NoteScale {
    const Scale* scale = nullptr;
    int          key   = 0;
    int snap(int midi) const { return scale ? scale_snap(*scale, key, midi) : midi; }
    bool contains(int midi) const { return !scale || scale_contains(*scale, key, midi); }
};
inline NoteScale phrase_note_scale(const Project& p, const Phrase& ph, int row, int instrument) {
    if (instrument >= 0 && instrument < static_cast<int>(p.instruments.size())) {
        const Instrument& ins = p.instruments[static_cast<size_t>(instrument)];
        if (!ins.transposeEnabled || note_selects_slice(ins, -1)) return {};
    }
    const ScaleAt at = phrase_scale_at_row(ph, row, p.scaleKey);
    return {&scale_at(p, at.slot), at.key};
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
