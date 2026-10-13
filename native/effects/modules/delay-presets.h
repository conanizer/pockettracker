#pragma once

// ───────────────────────────────────────────────────────────────────────────
// The delay's built-in PRESETS — three names for three sets of cell values, and nothing else.
//
// ⚠️⚠️ **A PRESET'S NAME IS NOT AN ARGUMENT TO THE DSP.** A project holds PONG, TONE and WOBL; the
// preset cell writes all three at once and the project keeps the name only as a label
// (ui/fx_preset_bank.h). A preset is a starting place, not a mode.
// ⚠️ A built-in row leaves TIME, FDBK and REV alone; a saved file (ui/fx_preset_io.h) carries them.
//
// ⚠️ NORMAL'S ROW IS ALSO THE STRUCT DEFAULT (model.h): pong off, TONE FF, WOBL 00 — what a project
// without these cells loads as, and it must sound as it always did, so each is gated OFF at its
// default in delay-module.h. Other rows may be re-tuned freely; NORMAL's may not.
//
// This header has no includes on purpose: the UI reads it (for the names and the match) and so does
// the DSP, and pulling in the delay module would drag `EqModule` into every UI translation unit,
// where `pt::ui::EqModule` already lives.

struct DelayPreset {
    const char* name;
    bool        pong;     // repeats come back on the other side
    int         tone;     // 00-FF, FF = fully open (no filter on the repeats at all)
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

/** Which built-in preset these three cells are, or −1 when they are nobody's. */
inline int delay_preset_match(bool pong, int tone, int wobble) {
    for (int i = 0; i < kDelayPresetCount; ++i) {
        const DelayPreset& d = kDelayPresets[i];
        if (d.pong == pong && d.tone == tone && d.wobble == wobble) return i;
    }
    return -1;
}
