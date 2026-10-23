#pragma once

// The delay's built-in presets: names for sets of cell values. The DSP never sees the name.
// Row 0 is what a new project opens on (songcore::new_project). The struct defaults in model.h are
// what older files play with and match no row.
// No includes: the UI reads this too, and the delay module would clash with its EqModule.

struct DelayPreset {
    const char* name;
    bool        pong;        // repeats come back on the other side
    bool        sync;        // `time` is a note division (0..B), else 00-FF
    int         time;
    int         feedback;    // 00-FF, FF = the repeats never fade
    int         tone;        // 00-FF, FF = fully open (no filter on the repeats at all)
    int         wobble;      // 00-FF, 00 = the read head does not drift
    int         reverbSend;  // 00-FF, how much of the delay goes on into the reverb
};

inline constexpr DelayPreset kDelayPresets[] = {
    {"DIGITAL", false, false, 0x30, 0x30, 0xFF, 0x00, 0x00},
    {"SLAP",    false, false, 0x0C, 0x00, 0x20, 0x80, 0x10},
    {"TAPE",    false, false, 0x20, 0x60, 0x30, 0xA0, 0x00},
    {"PONG",    true,  false, 0x20, 0x80, 0x40, 0x80, 0x10},
    {"DUB",     false, false, 0x28, 0xF0, 0x30, 0xFF, 0x0A},
    {"RESO",    false, false, 0x04, 0xFF, 0x22, 0xFF, 0x00},
    {"FOREVER", false, false, 0x30, 0xFF, 0x80, 0xFF, 0x00},
};
inline constexpr int kDelayPresetCount = 7;

// Which built-in preset these cells match, or −1.
inline int delay_preset_match(bool pong, bool sync, int time, int feedback, int tone, int wobble,
                              int reverbSend) {
    for (int i = 0; i < kDelayPresetCount; ++i) {
        const DelayPreset& d = kDelayPresets[i];
        if (d.pong == pong && d.sync == sync && d.time == time && d.feedback == feedback &&
            d.tone == tone && d.wobble == wobble && d.reverbSend == reverbSend)
            return i;
    }
    return -1;
}
