#pragma once

// The reverb's built-in presets (names for sets of cell values) and its algorithm list. The DSP
// never sees a preset's name.
// Row 0 is what a new project opens on (songcore::new_project). The struct defaults in model.h are
// what older files play with and match no row.
// ⚠️ Only PRE and WIDE are gated out at their defaults; SIZE is neutral at 60 by construction (scale
// exactly 1). DCAY, DAMP and MOD have no neutral value.
// Only <cmath>: the UI reads this too, and the reverb module would clash with its EqModule.
#include <cmath>

struct ReverbPreset {
    const char* name;
    int         algo;     // which reverb reads the rest — the project's `reverbAlgo`
    int         decay;    // 00-FF, the tail's length — the project's `reverbFeedback`
    int         room;     // 00-FF, the room's dimensions — the project's `reverbSize`; 60 = as shipped
    int         damp;     // 00-FF, how bright it stays
    int         pre;      // 00-FF, the gap before it starts; 00 = none
    int         width;    // 00-FF, 80 = the stereo image untouched
    int         mod;      // 00-FF, 40 = the wander the algorithm has always had; 00 = none
};

// ALGO, the project's `reverbAlgo`. ⚠️ The number is what a project stores: append, never insert;
// 0 is the original reverb, which an older project loads as. Each algorithm reads the same cells its
// own way (dragonfly-reverb.cpp).
inline constexpr int kReverbAlgoOld   = 0;   // ReverbSc — every cell
inline constexpr int kReverbAlgoHall  = 1;   // Dragonfly Hall — every cell
inline constexpr int kReverbAlgoRoom  = 2;   // Dragonfly Room — every cell
inline constexpr int kReverbAlgoPlate = 3;   // Dragonfly Plate, "Nested" — SIZE and MOD read nothing
inline constexpr int kReverbAlgoFoil  = 4;   // Dragonfly Plate, "Simple" — SIZE and MOD read nothing
inline constexpr int kReverbAlgoCount = 5;

// False for the two plates: no room to size, nothing that moves.
inline constexpr bool reverb_algo_reads_size_and_mod(int algo) {
    return algo != kReverbAlgoPlate && algo != kReverbAlgoFoil;
}

inline const char* reverb_algo_name(int algo) {
    static constexpr const char* kNames[kReverbAlgoCount] = {"OLD", "HALL", "ROOM", "PLATE", "FOIL"};
    return (algo >= 0 && algo < kReverbAlgoCount) ? kNames[algo] : kNames[0];
}

inline constexpr ReverbPreset kReverbPresets[] = {
    //              algo                decay room  damp  pre   width mod
    {"DEFAULT",     kReverbAlgoOld,     0x80, 0x80, 0x80, 0x20, 0xFF, 0x20},
    {"ROOM",        kReverbAlgoHall,    0x10, 0x20, 0x40, 0x20, 0x80, 0x00},
    {"ROOM_GLUE",   kReverbAlgoRoom,    0x10, 0x20, 0x30, 0x10, 0xA0, 0x30},
    {"HALL_BRIGHT", kReverbAlgoHall,    0x80, 0xA0, 0x80, 0x80, 0xFF, 0x20},
    {"HALL_WASH",   kReverbAlgoHall,    0xE0, 0x40, 0x20, 0x80, 0xFF, 0x10},
    {"PLATE_SHORT", kReverbAlgoPlate,   0x60, 0x00, 0x40, 0x20, 0x80, 0x00},
    {"FOIL_MONO",   kReverbAlgoFoil,    0x40, 0x00, 0xA0, 0x10, 0x00, 0x00},
};
inline constexpr int kReverbPresetCount = 7;

// Which built-in preset these cells match, or −1.
inline int reverb_preset_match(int algo, int decay, int room, int damp, int pre, int width, int mod) {
    for (int i = 0; i < kReverbPresetCount; ++i) {
        const ReverbPreset& r = kReverbPresets[i];
        if (r.algo == algo && r.decay == decay && r.room == room && r.damp == damp &&
            r.pre == pre && r.width == width && r.mod == mod)
            return i;
    }
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

// Wet gain that undoes DCAY's effect on level (the output scales with the feedback), so DCAY sets
// the length and the REV fader the loudness. The exponent is a fit; `g` is capped because FF zeroes it.
inline float reverb_decay_gain(int decayHex, int sizeHex = 0x60) {
    auto level = [](float g) {
        if (g > 0.99f) g = 0.99f;
        return g * powf(1.0f - g * g, -0.32f);
    };
    return level(reverb_decay_feedback(0x60)) / level(reverb_decay_feedback(decayHex, sizeHex));
}
