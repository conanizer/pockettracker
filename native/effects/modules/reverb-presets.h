#pragma once

// The reverb's built-in presets (names for sets of cell values) and its algorithm list. The DSP
// never sees a preset's name.
// Row 0 is what a new project opens on (songcore::new_project). The struct defaults (send_cells.h)
// are what older files play with and match no row.
// ⚠️ Only PRE and WIDE are gated out at their defaults; SIZE is neutral at 60 by construction (scale
// exactly 1). DCAY, DAMP and MOD have no neutral value.
// Only <cmath> and the cells: the UI reads this too, and the reverb module would clash with its EqModule.
#include <cmath>

#include "../../songcore/send_cells.h"

struct ReverbPreset {
    const char*           name;
    songcore::ReverbCells cells;
};

// ALGO, the cells' `algo`. ⚠️ The number is what a project stores: append, never insert;
// 0 is the original reverb, which an older project loads as. Each algorithm reads the same cells its
// own way (dragonfly-reverb.cpp).
inline constexpr int kReverbAlgoDefault = 0;   // ReverbSc — every cell
inline constexpr int kReverbAlgoHall    = 1;   // Dragonfly Hall — every cell
inline constexpr int kReverbAlgoRoom    = 2;   // Dragonfly Room — every cell
inline constexpr int kReverbAlgoPlate   = 3;   // Dragonfly Plate, "Nested" — SIZE and MOD read nothing
inline constexpr int kReverbAlgoFoil    = 4;   // Dragonfly Plate, "Simple" — SIZE and MOD read nothing
inline constexpr int kReverbAlgoCount   = 5;

// False for the two plates: no room to size, nothing that moves.
inline constexpr bool reverb_algo_reads_size_and_mod(int algo) {
    return algo != kReverbAlgoPlate && algo != kReverbAlgoFoil;
}

inline const char* reverb_algo_name(int algo) {
    static constexpr const char* kNames[kReverbAlgoCount] = {"DEFAULT", "HALL", "ROOM", "PLATE", "FOIL"};
    return (algo >= 0 && algo < kReverbAlgoCount) ? kNames[algo] : kNames[0];
}

inline constexpr ReverbPreset kReverbPresets[] = {
    //               algo               size  decay damp  pre   width mod
    {"DEFAULT",     {kReverbAlgoDefault,    0x80, 0x80, 0x80, 0x20, 0xFF, 0x20}},
    {"ROOM",        {kReverbAlgoHall,   0x20, 0x10, 0x40, 0x20, 0x80, 0x00}},
    {"ROOM_GLUE",   {kReverbAlgoRoom,   0x20, 0x10, 0x30, 0x10, 0xA0, 0x30}},
    {"HALL_BRIGHT", {kReverbAlgoHall,   0xA0, 0x80, 0x80, 0x80, 0xFF, 0x20}},
    {"HALL_WASH",   {kReverbAlgoHall,   0x40, 0xE0, 0x20, 0x80, 0xFF, 0x10}},
    {"PLATE_SHORT", {kReverbAlgoPlate,  0x00, 0x60, 0x40, 0x20, 0x80, 0x00}},
    {"FOIL_MONO",   {kReverbAlgoFoil,   0x00, 0x40, 0xA0, 0x10, 0x00, 0x00}},
};
inline constexpr int kReverbPresetCount = 7;

// Which built-in preset these cells match, or −1.
inline int reverb_preset_match(const songcore::ReverbCells& cells) {
    for (int i = 0; i < kReverbPresetCount; ++i)
        if (kReverbPresets[i].cells == cells) return i;
    return -1;
}

// MOD → ReverbSc's `SetPitchMod`: 0x40 is exactly 1.0, the native wander; FF just under 4, the most
// the lines are sized for.
inline float reverb_mod_scale(int modHex) { return modHex / 64.0f; }

// SIZE → how much every delay line stretches: 0.6× at 00, exactly 1 at 60, ~2.33× at FF.
// ⚠️ FF must stay under `DSY_REVERBSC_MAX_ROOM`; above it `SetRoom` clamps silently.
inline float reverb_room_scale(int sizeHex) { return powf(0.6f, (0x60 - sizeHex) / 96.0f); }

// SIZE → the wander's speed: 1 up to 60, falling to a third at FF. It only ever slows — a fast
// wander on a short tail warbles. `SetRoom` deepens it as the room grows.
inline constexpr float kReverbSlowestModRate = 1.0f / 3.0f;
inline float reverb_mod_rate(int sizeHex) {
    if (sizeHex <= 0x60) return 1.0f;
    return powf(kReverbSlowestModRate, (sizeHex - 0x60) / 159.0f);
}

// DCAY and DAMP are musical quantities, mapped onto the algorithm's numbers.
// ⚠️ Never send either raw: feedback and the corner are logarithmic to the ear.

// The tail's average loop time in the default room — fitted to the algorithm's measured decay, since
// the coupled lines' lengths do not predict it.
inline constexpr float kReverbLoopSeconds = 0.058f;

// DCAY → the tail's −60 dB time: 0.4 s at 00, ~25 s at FE.
inline float reverb_decay_seconds(int decayHex) { return 0.4f * powf(62.5f, decayHex / 255.0f); }

// DCAY and SIZE → ReverbSc's feedback. The room scales the loop time, so DCAY's length holds in any
// room. FF is exactly 1, a tail that never ends.
inline float reverb_decay_feedback(int decayHex, int sizeHex = 0x60) {
    if (decayHex >= 0xFF) return 1.0f;
    return powf(10.0f, -3.0f * kReverbLoopSeconds * reverb_room_scale(sizeHex) /
                           reverb_decay_seconds(decayHex));
}

// DAMP → the tail's low-pass corner, 1.2 kHz to 20 kHz; below ~1 kHz it changes only the level.
inline float reverb_damp_freq(int dampHex) { return 1200.0f * powf(16.667f, dampHex / 255.0f); }

// The same two cells as the Dragonfly algorithms read them. FF is DEFAULT's freeze and has no time there,
// so it reads as FE; DAMP is held to the 1-16 kHz the plugins' own damping knobs span.
inline double reverb_df_decay_seconds(int decayHex) { return reverb_decay_seconds(decayHex < 0xFE ? decayHex : 0xFE); }
inline double reverb_df_damp_hz(int dampHex) { return fmin(fmax(double(reverb_damp_freq(dampHex)), 1000.0), 16000.0); }

// PRE → the gap before the tail, 0 to 150 ms.
inline constexpr float kReverbPreDelayMaxSeconds = 0.15f;
inline float reverb_pre_delay_seconds(int preHex) { return (preHex / 255.0f) * kReverbPreDelayMaxSeconds; }

// Wet gain that undoes DCAY's effect on level (the output scales with the feedback), so DCAY sets
// the length and the REV fader the loudness. The exponent is a fit; `g` is capped because FF zeroes it.
inline float reverb_decay_gain(int decayHex, int sizeHex = 0x60) {
    auto level = [](float g) {
        if (g > 0.99f) g = 0.99f;
        return g * powf(1.0f - g * g, -0.32f);
    };
    return level(reverb_decay_feedback(0x60)) / level(reverb_decay_feedback(decayHex, sizeHex));
}
