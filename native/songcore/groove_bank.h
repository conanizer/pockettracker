#ifndef POCKETTRACKER_SONGCORE_GROOVE_BANK_H
#define POCKETTRACKER_SONGCORE_GROOVE_BANK_H

// ─── The factory groove bank ──────────────────────────────────────────────────────────────────────
//
// The shapes A+LEFT/RIGHT cycles on the GROOVE screen's name cell, also written to
// `<card>/Grooves/` as `.ptg` files when that folder is empty. Compiled in so the cycle works on an
// empty card; a display order, not an identity, so rows may be reordered.
//
// A pair of sixteenths is 24 tics, and the classic swing amounts (50 · 54 · 58 · 62 · 66 · 71 · 75 %)
// are exactly its integer splits — one tic per rung.
// `0D 0D 0B 0B` swings the eighths; `0D 0C 0B 0C` looks similar but is a 52 % sixteenth pattern.
// The last four are not swing: they are for `GRV`, turning rows into triplets or half/double time.
//
// ⚠️ A cycle must fill `rows × 12` tics, or the phrase runs a different length from every ungrooved
// track. A `0` step skips its row at no time cost — that is how triplets fit:
//
//   TRIPLET 8   16+16+16 = 48 tics over 4 rows  ✓
//   TRIPLET 4   32+32+32 = 96 tics over 8 rows  ✓
//
// No sixteenth-triplet row is possible: three would need to fit in two rows, and a `0` can only add
// a row, never remove one.

#include <string>
#include <vector>

#include "model.h"
#include "timing.h"  // TICS_PER_STEP, groove_active_length

namespace songcore {

// ⚠️ Every number below is in 48 PPQ; a change of PPQ must convert this table.
static_assert(TICS_PER_STEP == 12, "the factory groove bank is written in tics of a 48-PPQ step");

/** One factory groove: a name and its active steps. Everything after them is the end marker. */
struct GrooveBankEntry {
    const char*      name;
    std::vector<int> steps;
};

/** The bank. Step lists are distinct, or two rows would be indistinguishable to the cycle. */
inline const std::vector<GrooveBankEntry>& groove_bank() {
    static const std::vector<GrooveBankEntry> kBank = {
        {"STRAIGHT",   {12, 12}},                  // 50 % — and what a new groove is born with
        {"16TH 54",    {13, 11}},
        {"16TH 58",    {14, 10}},
        {"16TH 62",    {15,  9}},
        {"16TH 66",    {16,  8}},                  // perfect triplet swing
        {"16TH 71",    {17,  7}},
        {"16TH 75",    {18,  6}},                  // the far end; the MPC stops here too
        {"8TH 54",     {13, 13, 11, 11}},
        {"8TH 58",     {14, 14, 10, 10}},
        {"8TH 62",     {15, 15,  9,  9}},
        {"8TH 66",     {16, 16,  8,  8}},
        {"8TH 71",     {17, 17,  7,  7}},
        {"8TH 75",     {18, 18,  6,  6}},
        {"TRIPLET 8",  {16, 16, 16, 0}},              // three to the beat, in four rows
        {"TRIPLET 4",  {32, 32, 32, 0, 0, 0, 0, 0}},  // three across two beats, in eight rows
        {"HALFTIME",   {24, 24}},                  // every step an eighth
        {"DOUBLETIME", {6, 6}},                    // every step a thirty-second
    };
    return kBank;
}

/** A groove's active steps — those before the first end marker — as the bank writes them. */
inline std::vector<int> groove_active_steps(const Groove& g) {
    const int n = groove_active_length(g);  // the sequencer's own answer, never a second one
    return std::vector<int>(g.steps.begin(), g.steps.begin() + n);
}

inline int groove_bank_index_by_steps(const std::vector<int>& steps) {
    const std::vector<GrooveBankEntry>& bank = groove_bank();
    for (size_t i = 0; i < bank.size(); ++i)
        if (bank[i].steps == steps) return static_cast<int>(i);
    return -1;
}

inline int groove_bank_index_by_name(const std::string& name) {
    if (name.empty()) return -1;
    const std::vector<GrooveBankEntry>& bank = groove_bank();
    for (size_t i = 0; i < bank.size(); ++i)
        if (name == bank[i].name) return static_cast<int>(i);
    return -1;
}

/** Replace a slot's steps with a factory groove's, and take its name. Everything past them is blank. */
inline void groove_apply_bank(Groove& g, int index) {
    const std::vector<GrooveBankEntry>& bank = groove_bank();
    if (index < 0 || index >= static_cast<int>(bank.size())) return;
    const GrooveBankEntry& e = bank[static_cast<size_t>(index)];
    g.steps.assign(16, -1);
    for (size_t i = 0; i < e.steps.size() && i < 16; ++i) g.steps[i] = e.steps[i];
    g.name = e.name;
}

/**
 * The name to SHOW for a slot: its stored name, else the bank name its steps match (so a new groove
 * reads STRAIGHT), else empty. Never written back.
 */
inline std::string groove_display_name(const Groove& g) {
    if (!g.name.empty()) return g.name;
    const int idx = groove_bank_index_by_steps(groove_active_steps(g));
    return idx >= 0 ? std::string(groove_bank()[static_cast<size_t>(idx)].name) : std::string();
}

/** Has a slot drifted from the factory groove it is named after? (the `*` on screen). False for a
 *  name the bank does not know. */
inline bool groove_differs_from_its_name(const Groove& g) {
    const int idx = groove_bank_index_by_name(g.name);
    return idx >= 0 && groove_bank()[static_cast<size_t>(idx)].steps != groove_active_steps(g);
}

/**
 * The bank row the cycle steps FROM: the entry the name claims, else the one the steps match, else 0
 * — so A+LEFT and A+RIGHT are inverses even from a groove the bank lacks.
 */
inline int groove_bank_cycle_index(const Groove& g) {
    int idx = groove_bank_index_by_name(g.name);
    if (idx < 0) idx = groove_bank_index_by_steps(groove_active_steps(g));
    return idx < 0 ? 0 : idx;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_GROOVE_BANK_H
