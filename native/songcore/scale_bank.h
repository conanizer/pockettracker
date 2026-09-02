#ifndef POCKETTRACKER_SONGCORE_SCALE_BANK_H
#define POCKETTRACKER_SONGCORE_SCALE_BANK_H

// ─── The factory scale bank ───────────────────────────────────────────────────────────────────────
//
// The named shapes A+LEFT/RIGHT cycles on the SCALE screen's top row, and the same list the app writes
// to `<card>/Scales/` as `.pts` files the first time it finds that folder without any.
//
// ⚠️ COMPILED IN, NOT LOADED. The cycle has to work on a fresh card, on a card someone has emptied, and
// on the first launch before anything has been written — so the list a user picks from can never be a
// list that has to be read off storage first. The files are the SHAREABLE copy of it, not its source.
//
// ⚠️ THIS IS A DISPLAY ORDER, NOT AN IDENTITY. Unlike `EFFECT_TYPES`, nothing stores an index into it:
// a scale slot stores its twelve bits, and a `.pts` stores them too. So rows may be reordered or
// inserted freely — which is why it is sorted for a musician (the modes together, the pentatonics
// together) rather than kept in the order of the sheet it was transcribed from.
//
// ⚠️ A MASK IS COUNTED FROM THE KEY, NOT FROM C. Bit k set = the note k semitones above the key is in
// the scale, so one entry describes a shape the KEY then positions — the same relationship `Scale`
// itself has, and the reason bit 0 is set in every row here.
//
// ⭐ CHROMATIC IS INDEX 0 AND THAT IS NOT DECORATION: a default-constructed slot has all twelve degrees
// enabled, so slot 00 of a project that has never been touched already IS this entry. "Reset this slot"
// and "load Chromatic" are therefore the same gesture, and nothing had to be added to make it so.

#include <string>
#include <vector>

#include "model.h"
#include "scales.h"   // scale_mask — one answer to "which intervals is this slot", not two

namespace songcore {

/** One factory shape: a name, and which of the twelve intervals above the key it contains. */
struct ScaleBankEntry {
    const char* name;
    unsigned    mask;
};

/**
 * The bank. 38 shapes, every mask distinct — a duplicate would be two rows the cycle cannot tell apart
 * and two `.pts` files carrying the same scale under different names.
 *
 * ⚠️ Two rows differ DELIBERATELY from the sheet these were transcribed from, and both are recorded in
 * `docs/internal/scales.md`: TODI carries the real thaat (flat 2nd, SHARP 4th, flat 6th, natural 7th)
 * rather than the sheet's row, which is Phrygian spelled a second time; and HALF WHOLE is here because
 * "Diminished" names two different scales and the sheet only had one of them.
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

/**
 * The bank entry with this exact name, or −1.
 *
 * Exact and case-sensitive, because the only names that reach it are ones this table wrote — through
 * the cycle, or through a `.pts` seeded from it. A name the user typed is expected NOT to match.
 */
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

/**
 * Overwrite `s`'s twelve degrees and its name from bank entry `index`.
 *
 * ⚠️ The slot's `id` and its microtuning `offset` are LEFT ALONE. The id is *which slot this is*, and
 * the offsets are a separate edit made to the same slot rather than part of the shape a name describes.
 */
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
 * The name to SHOW for a slot, which is not always the name it stores.
 *
 * A slot that has never been named still has a shape, and on a fresh project that shape is Chromatic —
 * so the row would otherwise read blank on every untouched project while describing something the bank
 * has a perfectly good word for. An unnamed slot is therefore named by its INTERVALS; a named one is
 * named by its name, whatever its intervals have since become. Empty only when the slot is both unnamed
 * and a shape the bank does not contain, which is a scale built by hand.
 *
 * ⚠️ It does NOT write the name back. Adopting a derived name would put a `name` field into the file of
 * every project that has never opened this screen, and the scale pool is omitted from a `.ptp` whole
 * precisely so that those projects stay byte-identical.
 */
inline std::string scale_display_name(const Scale& s) {
    if (!s.name.empty()) return s.name;
    const int idx = scale_bank_index_by_mask(scale_mask(s));
    return idx >= 0 ? std::string(scale_bank()[static_cast<size_t>(idx)].name) : std::string();
}

/**
 * Has a slot drifted from the factory shape whose name it carries? The `*` the screen draws.
 *
 * ⚠️ False for a name the bank does not know — a scale the user named MYSCALE makes no claim about a
 * factory shape, so there is nothing for it to have drifted from and no star to earn.
 */
inline bool scale_differs_from_its_name(const Scale& s) {
    const int idx = scale_bank_index_by_name(s.name);
    return idx >= 0 && scale_bank()[static_cast<size_t>(idx)].mask != scale_mask(s);
}

/**
 * The bank row the cycle steps FROM: the entry this slot's name claims, else the entry its intervals
 * match, else 0.
 *
 * ⚠️ Falling back to 0 rather than to −1 is what makes A+LEFT and A+RIGHT exact inverses from a scale
 * the bank does not contain — both enter the ring at Chromatic and step away from it in the direction
 * pressed. (The THEME row, the precedent for this gesture, does NOT have that property: its two
 * expressions send an unknown palette to opposite ends of the list.)
 */
inline int scale_bank_cycle_index(const Scale& s) {
    int idx = scale_bank_index_by_name(s.name);
    if (idx < 0) idx = scale_bank_index_by_mask(scale_mask(s));
    return idx < 0 ? 0 : idx;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SCALE_BANK_H
