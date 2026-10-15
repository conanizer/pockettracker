#ifndef POCKETTRACKER_SONGCORE_GROOVE_BANK_H
#define POCKETTRACKER_SONGCORE_GROOVE_BANK_H

// The factory grooves the GROOVE screen's name cell cycles, also written to `<card>/Grooves/` when it
// is empty. A display order, not an identity. A pair of sixteenths is 24 tics, so each swing rung is
// one tic.
// ⚠️ A cycle must fill `rows × 12` tics, or the phrase runs a different length from ungrooved tracks.
// A `0` step skips its row at no cost — how triplets fit (16+16+16 over 4 rows).

#include <string>
#include <vector>

#include "model.h"
#include "timing.h"  // groove_active_length, the rulers

namespace songcore {

// ⚠️ Every number below is in the default ruler, 12 tics to the step. A groove in another ruler is
// compared and filled through it (`groove_matches_bank`, `groove_apply_bank`).
static_assert(GROOVE_DEFAULT_RULER == 12, "the factory groove bank is written 12 tics to the step");

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

/** Is `g` this factory groove, whatever ruler it is written in? `0D 0B` at 12 is `1A 16` at 24. */
inline bool groove_matches_bank(const Groove& g, const GrooveBankEntry& e) {
    const std::vector<int> steps = groove_active_steps(g);
    if (steps.size() != e.steps.size()) return false;
    const int ruler = groove_ticks_per_step(g);
    for (size_t i = 0; i < steps.size(); ++i)
        if (steps[i] * GROOVE_DEFAULT_RULER != e.steps[i] * ruler) return false;
    return true;
}

inline int groove_bank_index_by_steps(const Groove& g) {
    const std::vector<GrooveBankEntry>& bank = groove_bank();
    for (size_t i = 0; i < bank.size(); ++i)
        if (groove_matches_bank(g, bank[i])) return static_cast<int>(i);
    return -1;
}

inline int groove_bank_index_by_name(const std::string& name) {
    if (name.empty()) return -1;
    const std::vector<GrooveBankEntry>& bank = groove_bank();
    for (size_t i = 0; i < bank.size(); ++i)
        if (name == bank[i].name) return static_cast<int>(i);
    return -1;
}

// A factory groove into a slot, name and all; the slot keeps its ruler if the groove fits it exactly.
inline void groove_apply_bank(Groove& g, int index) {
    const std::vector<GrooveBankEntry>& bank = groove_bank();
    if (index < 0 || index >= static_cast<int>(bank.size())) return;
    const GrooveBankEntry& e = bank[static_cast<size_t>(index)];
    const int ruler = groove_ticks_per_step(g);

    Groove filled = g;
    filled.steps.assign(16, -1);
    for (size_t i = 0; i < e.steps.size() && i < 16; ++i) filled.steps[i] = e.steps[i];
    filled.ticksPerStep = GROOVE_DEFAULT_RULER;
    filled.name         = e.name;

    Groove kept = filled;
    g = (groove_set_ruler(kept, ruler) && groove_matches_bank(kept, e)) ? kept : filled;
}

// The name to show: the stored one, else the bank name its steps match, else empty.
inline std::string groove_display_name(const Groove& g) {
    if (!g.name.empty()) return g.name;
    const int idx = groove_bank_index_by_steps(g);
    return idx >= 0 ? std::string(groove_bank()[static_cast<size_t>(idx)].name) : std::string();
}

/** Has a slot drifted from the factory groove it is named after? (the `*` on screen). False for a
 *  name the bank does not know. */
inline bool groove_differs_from_its_name(const Groove& g) {
    const int idx = groove_bank_index_by_name(g.name);
    return idx >= 0 && !groove_matches_bank(g, groove_bank()[static_cast<size_t>(idx)]);
}

// The bank row the cycle steps from: by name, else by steps, else 0.
inline int groove_bank_cycle_index(const Groove& g) {
    int idx = groove_bank_index_by_name(g.name);
    if (idx < 0) idx = groove_bank_index_by_steps(g);
    return idx < 0 ? 0 : idx;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_GROOVE_BANK_H
