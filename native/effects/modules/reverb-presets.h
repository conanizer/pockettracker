#pragma once

// ───────────────────────────────────────────────────────────────────────────
// The reverb's PRESETS — four names for four sets of cell values — and the names of the algorithms
// that read those cells.
//
// ⚠️⚠️ **THE NAME IS NOT STORED IN THE PROJECT AND IS NOT AN ARGUMENT TO THE DSP.** A project holds
// DCAY, SIZE, DAMP, PRE, WIDE and MOD; the TYPE cell writes all six at once and then reads the name
// back by matching. Nothing downstream can ask "is this HALL?", because after one turn of any other
// cell the question has no answer — which is the point. A preset is a starting place, not a mode.
// This is the shape delay-presets.h already has, and the two are deliberately the same shape.
//
// ⚠️⚠️ **`NORMAL`'S ROW IS THE STRUCT DEFAULT** (model.h): DCAY 60, SIZE 60, DAMP 80, PRE 00, WIDE 80,
// MOD 10 — and a project written before these cells existed loads without them and lands on that
// row, so the row and the struct must be changed TOGETHER or a brand-new project reads as USER. It
// is the one row whose values are not free.
//
// ⚠️⚠️ **ONLY PRE AND WIDE ARE GATED OUT AT THEIR DEFAULTS**, which is what keeps an old project's
// pre-delay and stereo image exactly as they were. **DCAY, DAMP and MOD are not, and cannot be.**
// DCAY and DAMP spend the whole of 00–FF on tail lengths and brightnesses a musician would actually
// choose; MOD's default is low because a wander sized for a long tail is audible as detuning on a
// short one. SIZE is neutral at 60 by construction rather than by a gate: the room scale is exactly
// 1 there, so the lines are the lengths they always were.
//
// ⚠️ A preset row may be RE-TUNED freely — it is only a set of values a user can then edit.
//
// ⏸️ **PLATE IS DELIBERATELY ABSENT.** What makes a plate a plate is the DENSITY of its build-up, and
// this reverb's is eight delay lines: a "PLATE" here could only be a bright hall, and a preset list
// is a promise about what its names mean.
//
// ⚠️ This header pulls in `<cmath>` and NOTHING ELSE: the UI reads it (for the names and the match)
// and so does the DSP, and pulling in the reverb module would drag `EqModule` into every UI
// translation unit, where `pt::ui::EqModule` already lives.
#include <cmath>

struct ReverbPreset {
    const char* name;
    int         decay;    // 00-FF, the tail's length — the project's `reverbFeedback`
    int         room;     // 00-FF, the room's dimensions — the project's `reverbSize`; 60 = as shipped
    int         damp;     // 00-FF, how bright it stays
    int         pre;      // 00-FF, the gap before it starts; 00 = none
    int         width;    // 00-FF, 80 = the stereo image untouched
    int         mod;      // 00-FF, 40 = the wander the algorithm has always had; 00 = none
};

// ⚠️ **THE WANDER RISES WITH THE TAIL AND ONLY WITH IT.** A short tail is over before movement in it
// could be heard as anything but detuning, so ROOM has none at all; a 12-second one sits still and
// turns metallic without some, so CAVE has the most. It is the length that decides how much a row
// can carry, not how grand the name sounds.
//
// ⚠️ **SIZE follows the name, and the wander's SPEED follows SIZE** (`reverb_mod_rate`), so the bigger
// rows also drift more slowly without a column of their own for it.
inline constexpr ReverbPreset kReverbPresets[] = {
    // 1.9 s in the room the reverb has always had, and bright enough to sit under anything.
    {"NORMAL", 0x60, 0x60, 0x80, 0x00, 0x80, 0x10},
    // Tight, bright and close in: 0.6 s in a smaller room, almost no gap before it, a narrower image
    // than the others because a small room does not arrive from the sides, and a tail held still.
    {"ROOM",   0x18, 0x20, 0xA0, 0x04, 0x70, 0x00},
    // The gap is what makes it a hall — 30 ms of clear air before the tail, so the source stays in
    // front of it — and 3.6 s of darker, wider tail in a room half as large again.
    {"HALL",   0x88, 0xB0, 0x60, 0x33, 0xC0, 0x20},
    // 12 s, the darkest of the four, in nearly the largest room, with the widest image and the
    // deepest wander. FF is the freeze, and a preset is a starting place, not a held chord.
    {"CAVE",   0xD0, 0xF0, 0x20, 0x55, 0xFF, 0x40},
};
inline constexpr int kReverbPresetCount = 4;

// ───────────────────────────────────────────────────────────────────────────
// ALGO — which reverb reads the cells. The project's `reverbAlgo`.
//
// ⚠️⚠️ **A NUMBER HERE IS WHAT A PROJECT STORES. Append, never insert** — and 0 must go on meaning the
// reverb that shipped, because a project written before the cell existed loads as 0.
//
// ⚠️ The presets above do NOT write this, and choosing an algorithm writes no other cell: each
// algorithm reads the same DCAY/SIZE/DAMP/MOD its own way (dragonfly/dragonfly-reverb.cpp), so
// switching back returns exactly the sound that was there.
inline constexpr int kReverbAlgoOld   = 0;   // ReverbSc — every cell
inline constexpr int kReverbAlgoHall  = 1;   // Dragonfly Hall — every cell
inline constexpr int kReverbAlgoRoom  = 2;   // Dragonfly Room — every cell
inline constexpr int kReverbAlgoPlate = 3;   // Dragonfly Plate, "Nested" — SIZE and MOD read nothing
inline constexpr int kReverbAlgoFoil  = 4;   // Dragonfly Plate, "Simple" — SIZE and MOD read nothing
inline constexpr int kReverbAlgoTank  = 5;   // Dragonfly Plate, "Tank" — SIZE and MOD read nothing
inline constexpr int kReverbAlgoEarly = 6;   // Dragonfly Early Reflections — DCAY reads nothing,
                                             // MOD picks one of eight reflection programs
inline constexpr int kReverbAlgoCount = 7;

inline const char* reverb_algo_name(int algo) {
    static constexpr const char* kNames[kReverbAlgoCount] = {"OLD",  "HALL", "ROOM", "PLATE",
                                                             "FOIL", "TANK", "EARLY"};
    return (algo >= 0 && algo < kReverbAlgoCount) ? kNames[algo] : kNames[0];
}

// The index shown on the TYPE cell when no preset matches. It is one past the last, so the cell's
// range is 0..kReverbPresetCount while the cells are hand-set and 0..kReverbPresetCount-1 once a
// preset is chosen — which is what makes USER reachable to LEAVE and impossible to ENTER on purpose.
inline constexpr int kReverbPresetUser = kReverbPresetCount;

/** Which preset these cells are, or `kReverbPresetUser` when they are nobody's. */
inline int reverb_preset_match(int decay, int room, int damp, int pre, int width, int mod) {
    for (int i = 0; i < kReverbPresetCount; ++i) {
        const ReverbPreset& r = kReverbPresets[i];
        if (r.decay == decay && r.room == room && r.damp == damp && r.pre == pre &&
            r.width == width && r.mod == mod)
            return i;
    }
    return kReverbPresetUser;
}

/**
 * MOD's cell → ReverbSc's `SetPitchMod`. ⚠️ **THE DIVISOR IS 64 AND NOT 255 SO THAT 0x40 IS EXACTLY
 * 1.0** — 1.0 is the wander the algorithm was fixed at before the cell existed, and a default that
 * only came close to it would change the sound of every project ever saved. FF reaches just under
 * four, which is the ceiling the delay lines are sized for.
 */
inline float reverb_mod_scale(int modHex) { return modHex / 64.0f; }

// ───────────────────────────────────────────────────────────────────────────
// SIZE — the room's dimensions, and the wander's speed that follows them.

/**
 * SIZE 00-FF → how much every delay line is stretched, geometrically: 0.6× at 00, exactly 1 at 60,
 * about 2.33× at FF. ⚠️ **60 IS EXACTLY 1** — the exponent is zero there — so the default room is
 * the one the reverb always had, bit for bit. ⚠️ FF must stay under `DSY_REVERBSC_MAX_ROOM`, which the
 * lines are sized for; above it `SetRoom` clamps silently.
 *
 * The floor is where the eight lines, 24–52 ms at 00, still read as a room rather than a comb: much
 * shorter and the tail rings at the lines' own pitches.
 */
inline float reverb_room_scale(int sizeHex) { return powf(0.6f, (0x60 - sizeHex) / 96.0f); }

/**
 * SIZE 00-FF → how fast the wander moves, as a multiple of the speed the algorithm was built with
 * (each line picks a new target 0.9–4 times a second).
 *
 * ⚠️⚠️ **IT ONLY EVER SLOWS DOWN.** Up to 60 it is exactly 1 — a small room is not given a faster
 * wander, because a fast wander on a short tail is heard as a warble rather than as air. Above 60 it
 * falls geometrically to a third at FF. The depth grows with the room at the same time (`SetRoom`
 * scales it), so the amount of pitch drift stays about the same and only its speed changes.
 */
inline constexpr float kReverbSlowestModRate = 1.0f / 3.0f;
inline float reverb_mod_rate(int sizeHex) {
    if (sizeHex <= 0x60) return 1.0f;
    return powf(kReverbSlowestModRate, (sizeHex - 0x60) / 159.0f);
}

// ───────────────────────────────────────────────────────────────────────────
// DCAY and DAMP — the cell is a MUSICAL quantity, and the algorithm's number is derived from it.
//
// ⚠️⚠️ **NEITHER CELL MAY BE SENT TO THE ALGORITHM RAW.** `SetFeedback` takes a loop gain and
// `SetLpFreq` takes a corner, and both are logarithmic in what the ear hears: a cell wired straight
// to either spends most of its range on one end of the effect.

/** How long the eight delay lines take to be traversed once, on average, in the default room.
 *  ⚠️ **MEASURED AGAINST THE ALGORITHM'S OWN DECAY, NOT SUMMED OFF `kReverbParams`** — the lines are
 *  coupled by the scattering junction, so no single line's length predicts the rate the tail
 *  actually dies at. It is the one constant that turns a loop gain into a time and back. */
inline constexpr float kReverbLoopSeconds = 0.058f;

/** DCAY 00-FF → the tail's -60 dB time, geometrically: 0.4 s at 00, ~25 s at FE. */
inline float reverb_decay_seconds(int decayHex) { return 0.4f * powf(62.5f, decayHex / 255.0f); }

/**
 * DCAY and SIZE → ReverbSc's feedback.
 *
 * ⚠️⚠️ **IT TAKES THE ROOM TOO, AND THAT IS WHAT KEEPS THE TWO CELLS INDEPENDENT.** A loop gain is a
 * loss PER PASS, and a bigger room makes each pass longer — so the same gain in a room twice the size
 * rings twice as long. The loop time is scaled with the room before the gain is derived from it.
 *
 * ⚠️ **FF IS FOREVER AND IS THE ONE VALUE NOT ON THE CURVE** — a gain of exactly 1 is a tail that
 * never ends, whatever the room, and no finite decay time names it. FE is the longest one that does.
 */
inline float reverb_decay_feedback(int decayHex, int sizeHex = 0x60) {
    if (decayHex >= 0xFF) return 1.0f;
    return powf(10.0f, -3.0f * kReverbLoopSeconds * reverb_room_scale(sizeHex) /
                           reverb_decay_seconds(decayHex));
}

/** DAMP 00-FF → the corner of the low-pass inside the tail, 1.2 kHz to 20 kHz. ⚠️ The floor is
 *  1.2 kHz rather than the algorithm's own 0 because below about a kilohertz the filter stops
 *  changing the tail's COLOUR and only changes its LEVEL — the tail is already that dark. */
inline float reverb_damp_freq(int dampHex) { return 1200.0f * powf(16.667f, dampHex / 255.0f); }

/**
 * The wet gain that DCAY must not set. ⚠️⚠️ **THE REVERB'S OUTPUT IS BUILT ONLY OUT OF THE DELAY
 * LINES' OWN STATE — the dry signal never reaches it — so its LEVEL is proportional to the feedback,
 * and a short tail used to be a quiet one by about 19 dB across the cell.** This divides that level
 * back out, against the level the default cells sit at, so DCAY changes how LONG the tail is and the
 * REV fader on the MIXER screen changes how loud.
 *
 * ⚠️ The exponent is a FIT to the measured onset level, not a derivation — the level rises as
 * `g·(1-g²)^-0.32` because the damping filter takes a share of the loop that the gain alone does not
 * predict. It is read off the GAIN, so a bigger room, which needs a higher gain for the same tail,
 * is compensated through the same curve. ⚠️ `g` is capped before the second term: at FF it is exactly
 * 1, and `(1-g²)` there is zero, which would divide the reverb into silence at the freeze.
 */
inline float reverb_decay_gain(int decayHex, int sizeHex = 0x60) {
    auto level = [](float g) {
        if (g > 0.99f) g = 0.99f;
        return g * powf(1.0f - g * g, -0.32f);
    };
    return level(reverb_decay_feedback(0x60)) / level(reverb_decay_feedback(decayHex, sizeHex));
}
