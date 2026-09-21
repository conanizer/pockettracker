#ifndef POCKETTRACKER_SONGCORE_GROOVE_BANK_H
#define POCKETTRACKER_SONGCORE_GROOVE_BANK_H

// ─── The factory groove bank ──────────────────────────────────────────────────────────────────────
//
// The named shapes A+LEFT/RIGHT cycles on the GROOVE screen's name cell, and the same list the app
// writes to `<card>/Grooves/` as `.ptg` files the first time it finds that folder without any. Built
// the way `scale_bank.h` is built, for the same reasons — compiled in so the cycle works on an empty
// card, a display order rather than an identity, so rows may be reordered freely.
//
// ⭐⭐ THE SWING LADDER IS EXACTLY ONE TICK PER RUNG, AND THAT IS WHY THESE NUMBERS AND NOT OTHERS.
// A pair of sixteenths is 24 tics (TICS_PER_STEP × 2), so one tic is 4.17 % of the pair — and the
// swing amounts every drum machine since the MPC60 names, 50 · 54 · 58 · 62 · 66 · 71 · 75, are the
// seven integer splits of 24. So the ladder below is the real thing rather than an approximation of
// it, and nudging a pair by one tic on the GROOVE screen moves it exactly one named rung.
//
// ⚠️ THE EIGHTH ROWS KEEP THE SIXTEENTHS INSIDE EACH EIGHTH EVEN. `0D 0D 0B 0B` swings the EIGHTHS;
// `0D 0C 0B 0C` looks similar and does not — it is a 52 % sixteenth pattern.
//
// ⚠️ THE LAST FOUR ARE NOT SWING, and they are here for the `GRV` command rather than for this
// screen: assigning one mid-phrase turns the rows into triplets (or half/double time) without the
// phrase being retyped. Ordered last so the list says which kind of thing each row is.
//
// ⚠️⚠️ A GROOVE'S CYCLE MUST FILL THE SAME TIME AS THE ROWS IT COVERS, AND THE `0` STEPS ARE WHAT
// BUY THAT. A phrase counts FOUR rows to a beat whatever groove is on it, so a cycle that does not
// come out to `rows × 12` tics makes the phrase a different length from every ungrooved track and
// the song comes apart. `0` means *skip this row*: it costs no time, so it is the one way to spend
// rows without spending tics.
//
//   TRIPLET 8   3 notes fill 1 beat  →  16+16+16 = 48 tics over 4 rows = 4 × 12  ✓
//   TRIPLET 4   3 notes fill 2 beats →  32+32+32 = 96 tics over 8 rows = 8 × 12  ✓
//
// ⚠️⚠️ AND THAT IS WHY THERE IS NO SIXTEENTH-TRIPLET ROW. Three of them fill an EIGHTH, which is two
// rows — so the cycle would need three sounding rows inside a two-row space. A `0` can only add a
// row, never take one away, so no padding exists that makes it fit. The shape is still reachable by
// typing tics by hand on a phrase whose length you have chosen to suit it; it is simply not
// something the list can hand you ready to use, and offering it would be offering a trap.

#include <string>
#include <vector>

#include "model.h"
#include "timing.h"  // TICS_PER_STEP, and groove_active_length — the one answer to "where does it end"

namespace songcore {

// ⚠️ EVERY NUMBER IN THE BANK BELOW IS WRITTEN IN 48 PPQ. The swing rungs are integer splits of 24
// tics and the triplets are exact thirds of 48; at any other tic count they are neither. Widening
// the ruler (the PPQ work) has to convert this table, not inherit it.
static_assert(TICS_PER_STEP == 12, "the factory groove bank is written in tics of a 48-PPQ step");

/** One factory groove: a name and its active steps. Everything after them is the end marker. */
struct GrooveBankEntry {
    const char*      name;
    std::vector<int> steps;
};

/**
 * The bank. Every entry's step list is distinct — a duplicate would be two rows the cycle cannot tell
 * apart and two `.ptg` files carrying the same groove under different names.
 */
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
 * The name to SHOW for a slot, which is not always the name it stores — `scale_display_name`'s twin,
 * and the same bargain. An unnamed slot is named by its STEPS, so a new groove reads STRAIGHT without
 * a `name` field ever being written; a named one keeps its name whatever its steps have become; empty
 * only when it is both unnamed and a shape the bank has no word for.
 *
 * ⚠️ It does NOT write the name back, so a project that has never opened this screen stays as it was.
 */
inline std::string groove_display_name(const Groove& g) {
    if (!g.name.empty()) return g.name;
    const int idx = groove_bank_index_by_steps(groove_active_steps(g));
    return idx >= 0 ? std::string(groove_bank()[static_cast<size_t>(idx)].name) : std::string();
}

/**
 * Has a slot drifted from the factory groove whose name it carries? The `*` the screen draws.
 *
 * ⚠️ False for a name the bank does not know — a groove the user saved as MYGROOVE makes no claim
 * about a factory shape, so there is nothing to have drifted from.
 */
inline bool groove_differs_from_its_name(const Groove& g) {
    const int idx = groove_bank_index_by_name(g.name);
    return idx >= 0 && groove_bank()[static_cast<size_t>(idx)].steps != groove_active_steps(g);
}

/**
 * The bank row the cycle steps FROM: the entry this slot's name claims, else the entry its steps
 * match, else 0 — so A+LEFT and A+RIGHT are exact inverses from a groove the bank does not contain,
 * both entering the ring at STRAIGHT. `scale_bank_cycle_index` carries the same note.
 */
inline int groove_bank_cycle_index(const Groove& g) {
    int idx = groove_bank_index_by_name(g.name);
    if (idx < 0) idx = groove_bank_index_by_steps(groove_active_steps(g));
    return idx < 0 ? 0 : idx;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_GROOVE_BANK_H
