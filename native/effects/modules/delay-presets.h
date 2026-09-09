#pragma once

// ───────────────────────────────────────────────────────────────────────────
// The delay's PRESETS — three names for three sets of cell values, and nothing else.
//
// ⚠️⚠️ **THE NAME IS NOT STORED IN THE PROJECT AND IS NOT AN ARGUMENT TO THE DSP.** A project holds
// PONG, TONE and WOBL; the TYPE cell writes all three at once and then reads the name back by
// matching. Nothing downstream can ask "is this TAPE?", because after one turn of any other cell the
// question has no answer — which is the point. A preset is a starting place, not a mode.
//
// ⚠️ **THE DELAY THAT SHIPPED IS `NORMAL`'S ROW, AND IT IS ALSO THE STRUCT DEFAULT** (model.h): pong
// off, TONE FF, WOBL 00. Every project written before these cells existed loads without them, lands
// on that row, and must sound exactly as it always did — so each of the three is gated OFF at its
// default in delay-module.h rather than merely small.
//
// ⚠️ A preset row may be RE-TUNED freely — it is only a set of values a user can then edit. What is
// NOT free is NORMAL's row, which is load-bearing above.
//
// This header has no includes on purpose: the UI reads it (for the names and the match) and so does
// the DSP, and pulling in the delay module would drag `EqModule` into every UI translation unit,
// where `pt::ui::EqModule` already lives.

struct DelayPreset {
    const char* name;
    bool        pong;     // repeats come back on the other side
    int         tone;     // 00-FF, FF = fully open (no filter in the regeneration path at all)
    int         wobble;   // 00-FF, 00 = the read head does not drift
};

inline constexpr DelayPreset kDelayPresets[] = {
    {"NORMAL", false, 0xFF, 0x00},
    {"PING",   true,  0xFF, 0x00},
    // ⚠️ TAPE is a NAME for dark, drifting repeats, not a tape model — there is no saturation and no
    // head bump here, only the filter and the drifting read head.
    {"TAPE",   false, 0x60, 0x40},
};
inline constexpr int kDelayPresetCount = 3;

// The index shown on the TYPE cell when no preset matches. It is one past the last, so the cell's
// range is 0..kDelayPresetCount while the cells are hand-set and 0..kDelayPresetCount-1 once a preset
// is chosen — which is what makes USER reachable to LEAVE and impossible to ENTER on purpose.
inline constexpr int kDelayPresetUser = kDelayPresetCount;

/** Which preset these three cells are, or `kDelayPresetUser` when they are nobody's. */
inline int delay_preset_match(bool pong, int tone, int wobble) {
    for (int i = 0; i < kDelayPresetCount; ++i) {
        const DelayPreset& d = kDelayPresets[i];
        if (d.pong == pong && d.tone == tone && d.wobble == wobble) return i;
    }
    return kDelayPresetUser;
}
