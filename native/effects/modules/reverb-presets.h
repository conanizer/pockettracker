#pragma once

// ───────────────────────────────────────────────────────────────────────────
// The reverb's PRESETS — four names for four sets of cell values, and nothing else.
//
// ⚠️⚠️ **THE NAME IS NOT STORED IN THE PROJECT AND IS NOT AN ARGUMENT TO THE DSP.** A project holds
// SIZE, DAMP, PRE, WIDE and MOD; the TYPE cell writes all five at once and then reads the name back
// by matching. Nothing downstream can ask "is this HALL?", because after one turn of any other cell
// the question has no answer — which is the point. A preset is a starting place, not a mode. This is
// the shape delay-presets.h already has, and the two are deliberately the same shape.
//
// ⚠️ **THE REVERB THAT SHIPPED IS `NORMAL`'S ROW, AND IT IS ALSO THE STRUCT DEFAULT** (model.h):
// SIZE 60, DAMP 80, PRE 00, WIDE 80, MOD 40. Every project written before the last three cells
// existed loads without them, lands on that row, and must sound exactly as it always did — so each of
// the three is gated OUT at its default in reverb-module.h rather than merely set small.
//
// ⚠️ A preset row may be RE-TUNED freely — it is only a set of values a user can then edit. What is
// NOT free is NORMAL's row, which is load-bearing above.
//
// ⏸️ **PLATE IS DELIBERATELY ABSENT.** What makes a plate a plate is the DENSITY of its build-up, and
// this reverb's is eight fixed delay lines: a "PLATE" here could only be a bright hall, and a preset
// list is a promise about what its names mean. It waits on an algorithm that has a density control.
//
// This header has no includes on purpose: the UI reads it (for the names and the match) and so does
// the DSP, and pulling in the reverb module would drag `EqModule` into every UI translation unit,
// where `pt::ui::EqModule` already lives.

struct ReverbPreset {
    const char* name;
    int         size;     // 00-FF, the tail's length
    int         damp;     // 00-FF, how bright it stays
    int         pre;      // 00-FF, the gap before it starts; 00 = none
    int         width;    // 00-FF, 80 = the stereo image untouched
    int         mod;      // 00-FF, 40 = the wander the algorithm has always had; 00 = none
};

inline constexpr ReverbPreset kReverbPresets[] = {
    {"NORMAL", 0x60, 0x80, 0x00, 0x80, 0x40},
    // Tight, bright and close in: a short tail, almost no gap before it, and a narrower image than
    // the others, because a small room does not arrive from the sides.
    {"ROOM",   0x38, 0xA0, 0x04, 0x70, 0x20},
    // The gap is what makes it a hall — 30 ms of clear air before the tail, so the source stays in
    // front of it — and the tail is long, dark and wide behind that.
    {"HALL",   0xB0, 0x60, 0x33, 0xC0, 0x50},
    // Longest and darkest, with the widest image and the deepest wander, which is what stops a tail
    // this long from sitting still and turning metallic.
    {"CAVE",   0xE0, 0x40, 0x55, 0xFF, 0x60},
};
inline constexpr int kReverbPresetCount = 4;

// The index shown on the TYPE cell when no preset matches. It is one past the last, so the cell's
// range is 0..kReverbPresetCount while the cells are hand-set and 0..kReverbPresetCount-1 once a
// preset is chosen — which is what makes USER reachable to LEAVE and impossible to ENTER on purpose.
inline constexpr int kReverbPresetUser = kReverbPresetCount;

/** Which preset these five cells are, or `kReverbPresetUser` when they are nobody's. */
inline int reverb_preset_match(int size, int damp, int pre, int width, int mod) {
    for (int i = 0; i < kReverbPresetCount; ++i) {
        const ReverbPreset& r = kReverbPresets[i];
        if (r.size == size && r.damp == damp && r.pre == pre && r.width == width && r.mod == mod)
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
