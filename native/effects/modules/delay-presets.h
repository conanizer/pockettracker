#pragma once

// The delay's built-in presets: names for sets of cell values. The DSP never sees the name.
// Row 0 is what a new project opens on (songcore::new_project). The struct defaults (send_cells.h)
// are what older files play with and match no row.
// Only the cells: the UI reads this too, and the delay module would clash with its EqModule.

#include "../../songcore/send_cells.h"

struct DelayPreset {
    const char*          name;
    songcore::DelayCells cells;   // pong, sync, time, feedback, tone, wobble, reverbSend
};

inline constexpr DelayPreset kDelayPresets[] = {
    {"DIGITAL", {false, false, 0x30, 0x30, 0xFF, 0x00, 0x00}},
    {"SLAP",    {false, false, 0x0C, 0x00, 0x20, 0x80, 0x10}},
    {"TAPE",    {false, false, 0x20, 0x60, 0x30, 0xA0, 0x00}},
    {"PONG",    {true,  false, 0x20, 0x80, 0x40, 0x80, 0x10}},
    {"DUB",     {false, false, 0x28, 0xF0, 0x30, 0xFF, 0x0A}},
    {"RESO",    {false, false, 0x04, 0xFF, 0x22, 0xFF, 0x00}},
    {"FOREVER", {false, false, 0x30, 0xFF, 0x80, 0xFF, 0x00}},
};
inline constexpr int kDelayPresetCount = 7;

// Which built-in preset these cells match, or −1.
inline int delay_preset_match(const songcore::DelayCells& cells) {
    for (int i = 0; i < kDelayPresetCount; ++i)
        if (kDelayPresets[i].cells == cells) return i;
    return -1;
}
