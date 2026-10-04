#ifndef POCKETTRACKER_SONGCORE_SCALE_BANK_H
#define POCKETTRACKER_SONGCORE_SCALE_BANK_H

// ─── The factory scale bank ───────────────────────────────────────────────────────────────────────
//
// The shapes A+LEFT/RIGHT cycles on the SCALE screen's top row, also written to `<card>/Scales/` as
// `.pts` files when that folder is empty. Compiled in, so the cycle works on a fresh or emptied card;
// the files are a shareable copy, not the source.
// A display order, not an identity: slots and `.pts` files store twelve bits, never an index, so rows
// may be reordered (they are grouped for musicians).
// A mask counts FROM THE KEY: bit k = k semitones above the key, so bit 0 is set in every row.
// Chromatic is index 0, and is exactly what a default slot holds — "reset" and "load Chromatic" are the
// same gesture.

#include <string>
#include <vector>

#include "model.h"
#include "scales.h"   // scale_mask

namespace songcore {

/** One factory shape: a name, and which of the twelve intervals above the key it contains. */
struct ScaleBankEntry {
    const char* name;
    unsigned    mask;
};

/**
 * The bank: 38 shapes, every mask distinct (a duplicate could not be told apart by the cycle).
 * TODI is the real thaat (♭2 ♯4 ♭6 ♮7); HALF WHOLE is here because "Diminished" names two scales.
 */
inline const std::vector<ScaleBankEntry>& scale_bank() {
    static const std::vector<ScaleBankEntry> kBank = {
        {"Chromatic",         0x0FFFu},   // 0 1 2 3 4 5 6 7 8 9 10 11
        {"Major",             0x0AB5u},   // 0 2 4 5 7 9 11
        {"Minor",             0x05ADu},   // 0 2 3 5 7 8 10
        {"Dorian",            0x06ADu},   // 0 2 3 5 7 9 10
        {"Phrygian",          0x05ABu},   // 0 1 3 5 7 8 10
        {"Lydian",            0x0AD5u},   // 0 2 4 6 7 9 11
        {"Mixolydian",        0x06B5u},   // 0 2 4 5 7 9 10
        {"Locrian",           0x056Bu},   // 0 1 3 5 6 8 10
        {"Lydian Minor",      0x05D5u},   // 0 2 4 6 7 8 10
        {"Phrygian Dominant", 0x05B3u},   // 0 1 4 5 7 8 10
        {"Melodic Minor",     0x0AADu},   // 0 2 3 5 7 9 11
        {"Harmonic Minor",    0x09ADu},   // 0 2 3 5 7 8 11
        {"BeBop Major",       0x0BB5u},   // 0 2 4 5 7 8 9 11
        {"BeBop Dorian",      0x06BDu},   // 0 2 3 4 5 7 9 10
        {"BeBop Mixolydian",  0x0EB5u},   // 0 2 4 5 7 9 10 11
        {"Blues Minor",       0x04E9u},   // 0 3 5 6 7 10
        {"Blues Major",       0x029Du},   // 0 2 3 4 7 9
        {"Pentatonic Minor",  0x04A9u},   // 0 3 5 7 10
        {"Pentatonic Major",  0x0295u},   // 0 2 4 7 9
        {"Hungarian Minor",   0x09CDu},   // 0 2 3 6 7 8 11
        {"Ukrainian",         0x06CDu},   // 0 2 3 6 7 9 10
        {"Marva",             0x0AD3u},   // 0 1 4 6 7 9 11
        {"Todi",              0x09CBu},   // 0 1 3 6 7 8 11
        {"Whole Tone",        0x0555u},   // 0 2 4 6 8 10
        {"Diminished",        0x0B6Du},   // 0 2 3 5 6 8 9 11   (whole-half)
        {"Half Whole",        0x06DBu},   // 0 1 3 4 6 7 9 10
        {"Super Locrian",     0x055Bu},   // 0 1 3 4 6 8 10
        {"Hirajoshi",         0x018Du},   // 0 2 3 7 8
        {"In Sen",            0x04A3u},   // 0 1 5 7 10
        {"Yo",                0x02A5u},   // 0 2 5 7 9
        {"Iwato",             0x0463u},   // 0 1 5 6 10
        {"Kumoi",             0x028Du},   // 0 2 3 7 9
        {"Overtone",          0x06D5u},   // 0 2 4 6 7 9 10
        {"Double Harmonic",   0x09B3u},   // 0 1 4 5 7 8 11
        {"Indian",            0x05B1u},   // 0 4 5 7 8 10
        {"Neapolitan",        0x0AABu},   // 0 1 3 5 7 9 11
        {"Neapolitan Minor",  0x09ABu},   // 0 1 3 5 7 8 11
        {"Enigmatic",         0x0D53u},   // 0 1 4 6 8 10 11
    };
    return kBank;
}

/** The entry with this exact (case-sensitive) name, or −1. Only names this table wrote reach it. */
inline int scale_bank_index_by_name(const std::string& name) {
    if (name.empty()) return -1;
    const std::vector<ScaleBankEntry>& bank = scale_bank();
    for (size_t i = 0; i < bank.size(); ++i)
        if (name == bank[i].name) return static_cast<int>(i);
    return -1;
}

/** The bank entry with this exact interval set, or −1. Masks are distinct, so the answer is unique. */
inline int scale_bank_index_by_mask(unsigned mask) {
    const std::vector<ScaleBankEntry>& bank = scale_bank();
    for (size_t i = 0; i < bank.size(); ++i)
        if (bank[i].mask == (mask & 0x0FFFu)) return static_cast<int>(i);
    return -1;
}

/** Overwrite `s`'s degrees and name from entry `index`. ⚠️ Its `id` and microtuning `offset` stay —
 *  they are not part of the shape a name describes. */
inline void scale_apply_bank(Scale& s, int index) {
    const std::vector<ScaleBankEntry>& bank = scale_bank();
    if (index < 0 || index >= static_cast<int>(bank.size())) return;
    const unsigned mask = bank[static_cast<size_t>(index)].mask;
    s.enabled.assign(12, 0);
    for (int d = 0; d < 12; ++d)
        if ((mask >> d) & 1u) s.enabled[static_cast<size_t>(d)] = 1;
    s.name = bank[static_cast<size_t>(index)].name;
}

/**
 * The name to SHOW for a slot: its stored name, else the bank name its intervals match (a fresh slot
 * reads Chromatic), else empty for a hand-built shape.
 * ⚠️ Never written back: the scale pool is omitted from an untouched .ptp, and must stay that way.
 */
inline std::string scale_display_name(const Scale& s) {
    if (!s.name.empty()) return s.name;
    const int idx = scale_bank_index_by_mask(scale_mask(s));
    return idx >= 0 ? std::string(scale_bank()[static_cast<size_t>(idx)].name) : std::string();
}

/** Has a slot drifted from the factory shape it is named after? (the `*`). False for a name the
 *  bank does not know. */
inline bool scale_differs_from_its_name(const Scale& s) {
    const int idx = scale_bank_index_by_name(s.name);
    return idx >= 0 && scale_bank()[static_cast<size_t>(idx)].mask != scale_mask(s);
}

/**
 * The bank row the cycle steps FROM: the entry the name claims, else the one the intervals match,
 * else 0 — so A+LEFT and A+RIGHT are exact inverses even from a shape the bank lacks.
 */
inline int scale_bank_cycle_index(const Scale& s) {
    int idx = scale_bank_index_by_name(s.name);
    if (idx < 0) idx = scale_bank_index_by_mask(scale_mask(s));
    return idx < 0 ? 0 : idx;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SCALE_BANK_H
