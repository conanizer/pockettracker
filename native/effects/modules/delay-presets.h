#pragma once

// The delay's built-in presets: names for sets of PONG/TONE/WOBL values. The DSP never sees the
// name; TIME, FDBK and REV are left alone.
// ⚠️ NORMAL's row is the struct default (model.h) and must stay the plain delay; the others may be
// re-tuned. No includes: the UI reads this too, and the delay module would clash with its EqModule.

struct DelayPreset {
    const char* name;
    bool        pong;     // repeats come back on the other side
    int         tone;     // 00-FF, FF = fully open (no filter on the repeats at all)
    int         wobble;   // 00-FF, 00 = the read head does not drift
};

inline constexpr DelayPreset kDelayPresets[] = {
    {"NORMAL", false, 0xFF, 0x00},
    {"PING",   true,  0xFF, 0x00},
    // Dark, drifting repeats — a name, not a tape model.
    {"TAPE",   false, 0x60, 0x40},
};
inline constexpr int kDelayPresetCount = 3;

// Which built-in preset these cells match, or −1.
inline int delay_preset_match(bool pong, int tone, int wobble) {
    for (int i = 0; i < kDelayPresetCount; ++i) {
        const DelayPreset& d = kDelayPresets[i];
        if (d.pong == pong && d.tone == tone && d.wobble == wobble) return i;
    }
    return -1;
}
