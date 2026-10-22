#ifndef POCKETTRACKER_SONGCORE_CHORDS_H
#define POCKETTRACKER_SONGCORE_CHORDS_H

// CHD xy: the row's note becomes a chord of four notes. x picks one of sixteen chords, y lays them out.
//   y 0-3  root position, then inversions: the lowest note up an octave, one step at a time
//          (a triad's 3 is root position an octave up)
//   y 4-7  the same, then drop-2: the second-highest note down an octave
//   y 8-B  the same, then drop-3: the third-highest note down an octave
//   y C-F  the same, then spread: the lowest note down an octave and the highest up an octave
// A triad's fourth note is its lowest an octave up, added after the inversion — so an inversion
// doubles its new bass rather than landing on the octave already there.
// ⚠️ A saved CHD stores the byte, so editing a chord's notes changes every song that uses it.

#include "scales.h"

namespace songcore {

constexpr int CHORD_MAX_NOTES = 4;

struct ChordShape {
    const char* name;
    int         count;                        // 3 = a triad, doubled to four
    int         semitones[CHORD_MAX_NOTES];   // above the played note, ascending
};

inline constexpr ChordShape CHORDS[16] = {
    {"MAJOR",      3, {0, 4, 7}},
    {"MINOR",      3, {0, 3, 7}},
    {"SUS2",       3, {0, 2, 7}},
    {"SUS4",       3, {0, 5, 7}},
    {"DIMINISHED", 3, {0, 3, 6}},
    {"AUGMENTED",  3, {0, 4, 8}},
    {"MAJOR 7",    4, {0, 4, 7, 11}},
    {"MINOR 7",    4, {0, 3, 7, 10}},
    {"DOMINANT 7", 4, {0, 4, 7, 10}},
    {"MAJOR 6",    4, {0, 4, 7, 9}},
    {"MINOR 6",    4, {0, 3, 7, 9}},
    {"ADD9",       4, {0, 4, 7, 14}},
    {"MINOR ADD9", 4, {0, 3, 7, 14}},
    {"MAJOR 9",    4, {0, 4, 11, 14}},   // no fifth: four notes is the limit
    {"MINOR 9",    4, {0, 3, 10, 14}},
    {"MINOR 11",   4, {0, 3, 10, 17}},
};

inline const char* chord_name(int value) { return CHORDS[(value >> 4) & 0x0F].name; }

// y's layout, as the hint names it: "INV 2", "DROP-2 INV 1", "SPREAD".
inline const char* chord_layout_name(int value) {
    static const char* const NAMES[16] = {
        "ROOT",   "INV 1",        "INV 2",        "INV 3",
        "DROP-2", "DROP-2 INV 1", "DROP-2 INV 2", "DROP-2 INV 3",
        "DROP-3", "DROP-3 INV 1", "DROP-3 INV 2", "DROP-3 INV 3",
        "SPREAD", "SPREAD INV 1", "SPREAD INV 2", "SPREAD INV 3",
    };
    return NAMES[value & 0x0F];
}

inline void chord_sort(int* v, int n) {
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && v[j - 1] > v[j]; --j) {
            const int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
        }
}

// The notes a CHD byte plays, in semitones from the row's note, ascending. Always four.
inline int chord_voicing(int value, int out[CHORD_MAX_NOTES]) {
    const ChordShape& c = CHORDS[(value >> 4) & 0x0F];
    const int base = c.count, inversion = value & 0x03, layout = (value >> 2) & 0x03;
    for (int i = 0; i < base; ++i) out[i] = c.semitones[i];
    for (int s = 0; s < inversion; ++s) {
        chord_sort(out, base);
        out[0] += 12;
    }
    chord_sort(out, base);
    if (base == 3) out[3] = out[0] + 12;
    const int n = CHORD_MAX_NOTES;
    if (layout == 1)      out[n - 2] -= 12;
    else if (layout == 2) out[n - 3] -= 12;
    else if (layout == 3) { out[0] -= 12; out[n - 1] += 12; }
    chord_sort(out, n);
    return n;
}

// What a CHD byte plays on `root` (MIDI, where the note sounds): each note pulled onto `scale` on its
// own, two that land together kept once. Semitones from `root`, ascending; returns how many.
// A null scale leaves them alone.
inline int chord_notes(int value, int root, const Scale* scale, int key, int out[CHORD_MAX_NOTES]) {
    int voicing[CHORD_MAX_NOTES] = {};
    const int n = chord_voicing(value, voicing);
    int count = 0;
    for (int i = 0; i < n; ++i) {
        int semis = voicing[i];
        const int pitch = root + semis;
        if (scale && pitch >= 0 && pitch <= 127) semis = scale_snap(*scale, key, pitch) - root;
        bool merged = false;
        for (int k = 0; k < count; ++k) merged |= out[k] == semis;
        if (!merged) out[count++] = semis;
    }
    chord_sort(out, count);
    return count;
}

// A strum's order: `out[i]` = when note i (ascending) sounds, 0 first. ARC's x: 0 up, 1 down, 2 up and
// down by turns (`pass` counts the track's strums so far), 3 shuffled — `roll(k)` returns 0..k-1.
template <typename Roll>
inline void strum_order(int mode, int n, int pass, Roll roll, int out[CHORD_MAX_NOTES]) {
    if (mode == 2) mode = pass & 1;
    for (int i = 0; i < n; ++i) out[i] = mode == 1 ? n - 1 - i : i;
    if (mode == 3)
        for (int i = n - 1; i > 0; --i) {
            const int j = roll(i + 1);
            const int t = out[i]; out[i] = out[j]; out[j] = t;
        }
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_CHORDS_H
