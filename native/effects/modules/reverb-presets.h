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
// ⚠️⚠️ **`NORMAL`'S ROW IS THE STRUCT DEFAULT** (model.h): SIZE 60, DAMP 80, PRE 00, WIDE 80,
// MOD 10 — and a project written before these cells existed loads without them and lands on that
// row, so the row and the struct must be changed TOGETHER or a brand-new project reads as USER.
//
// ⚠️⚠️ **ONLY PRE AND WIDE ARE GATED OUT AT THEIR DEFAULTS**, which is what keeps an old project's
// pre-delay and stereo image exactly as they were. **SIZE, DAMP and MOD are not, and cannot be.**
// SIZE and DAMP were re-scaled to spend the whole of 00–FF on tail lengths and brightnesses a
// musician would actually choose; MOD's default was lowered because a wander sized for a long tail
// is audible as detuning on a short one. A project saved before those moves plays longer, brighter
// and stiller than it was written. That is the deliberate trade: the old scales spent half their
// range on settings nothing could be sent to.
//
// ⚠️ A preset row may be RE-TUNED freely — it is only a set of values a user can then edit.
//
// ⏸️ **PLATE IS DELIBERATELY ABSENT.** What makes a plate a plate is the DENSITY of its build-up, and
// this reverb's is eight fixed delay lines: a "PLATE" here could only be a bright hall, and a preset
// list is a promise about what its names mean. It waits on an algorithm that has a density control.
//
// ⚠️ This header pulls in `<cmath>` and NOTHING ELSE: the UI reads it (for the names and the match)
// and so does the DSP, and pulling in the reverb module would drag `EqModule` into every UI
// translation unit, where `pt::ui::EqModule` already lives.
#include <cmath>

struct ReverbPreset {
    const char* name;
    int         size;     // 00-FF, the tail's length
    int         damp;     // 00-FF, how bright it stays
    int         pre;      // 00-FF, the gap before it starts; 00 = none
    int         width;    // 00-FF, 80 = the stereo image untouched
    int         mod;      // 00-FF, 40 = the wander the algorithm has always had; 00 = none
};

// ⚠️ **THE WANDER RISES WITH THE TAIL AND ONLY WITH IT.** A short tail is over before movement in it
// could be heard as anything but detuning, so ROOM has none at all; a 12-second one sits still and
// turns metallic without some, so CAVE has the most. It is the length that decides how much a row
// can carry, not how grand the name sounds.
inline constexpr ReverbPreset kReverbPresets[] = {
    // 1.9 s, and bright enough to sit under anything.
    {"NORMAL", 0x60, 0x80, 0x00, 0x80, 0x10},
    // Tight, bright and close in: 0.6 s, almost no gap before it, a narrower image than the others
    // because a small room does not arrive from the sides, and a tail held perfectly still.
    {"ROOM",   0x18, 0xA0, 0x04, 0x70, 0x00},
    // The gap is what makes it a hall — 30 ms of clear air before the tail, so the source stays in
    // front of it — and 3.6 s of darker, wider tail behind that.
    {"HALL",   0x88, 0x60, 0x33, 0xC0, 0x20},
    // 12 s, the darkest of the four, with the widest image and the deepest wander.
    {"CAVE",   0xD0, 0x20, 0x55, 0xFF, 0x40},
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

// ───────────────────────────────────────────────────────────────────────────
// SIZE and DAMP — the cell is a MUSICAL quantity, and the algorithm's number is derived from it.
//
// ⚠️⚠️ **NEITHER CELL MAY BE SENT TO THE ALGORITHM RAW.** `SetFeedback` takes a loop gain and
// `SetLpFreq` takes a corner, and both are logarithmic in what the ear hears: a cell wired straight
// to either spends most of its range on one end of the effect. Measured on the raw scales, SIZE
// 00–80 covered 0 to 0.6 s of tail while 80–FF covered 0.6 s to forever, and DAMP 00–70 was one
// brightness at five different volumes. Both halves are now spent on the useful side.

/** How long the eight delay lines take to be traversed once, on average. ⚠️ **MEASURED AGAINST THE
 *  ALGORITHM'S OWN DECAY, NOT SUMMED OFF `kReverbParams`** — the lines are coupled by the scattering
 *  junction, so no single line's length predicts the rate the tail actually dies at. It is the one
 *  constant that turns a loop gain into a time and back. */
inline constexpr float kReverbLoopSeconds = 0.058f;

/** SIZE 00-FF → the tail's -60 dB time, geometrically: 0.4 s at 00, ~25 s at FE. */
inline float reverb_size_seconds(int sizeHex) { return 0.4f * powf(62.5f, sizeHex / 255.0f); }

/**
 * SIZE 00-FF → ReverbSc's feedback. ⚠️ **FF IS FOREVER AND IS THE ONE VALUE NOT ON THE CURVE** — a
 * gain of exactly 1 is a tail that never ends, which is a thing people reach the top of the cell
 * for, and no finite decay time names it. FE is the longest one that does.
 */
inline float reverb_size_feedback(int sizeHex) {
    if (sizeHex >= 0xFF) return 1.0f;
    return powf(10.0f, -3.0f * kReverbLoopSeconds / reverb_size_seconds(sizeHex));
}

/** DAMP 00-FF → the corner of the low-pass inside the tail, 1.2 kHz to 20 kHz. ⚠️ The floor is
 *  1.2 kHz rather than the algorithm's own 0 because below about a kilohertz the filter stops
 *  changing the tail's COLOUR and only changes its LEVEL — the tail is already that dark. */
inline float reverb_damp_freq(int dampHex) { return 1200.0f * powf(16.667f, dampHex / 255.0f); }

/**
 * The wet gain that SIZE must no longer set. ⚠️⚠️ **THE REVERB'S OUTPUT IS BUILT ONLY OUT OF THE
 * DELAY LINES' OWN STATE — the dry signal never reaches it — so its LEVEL is proportional to the
 * feedback, and turning the tail short used to turn the reverb down with it by about 19 dB across
 * the cell.** That is why a short setting read as "no reverb" rather than as "a small room": it was
 * both. This divides that level back out, against the level the default cell sits at, so SIZE
 * changes how LONG the tail is and the REV fader on the MIXER screen changes how loud.
 *
 * ⚠️ The exponent is a FIT to the measured onset level, not a derivation — the level rises as
 * `g·(1-g²)^-0.32` because the damping filter takes a share of the loop that the gain alone does not
 * predict. ⚠️ `g` is capped before the second term: at FF it is exactly 1, and `(1-g²)` there is
 * zero, which would divide the reverb into silence at the one setting people reach for a freeze.
 */
inline float reverb_size_gain(int sizeHex) {
    auto level = [](int hex) {
        float g = reverb_size_feedback(hex);
        if (g > 0.99f) g = 0.99f;
        return g * powf(1.0f - g * g, -0.32f);
    };
    return level(0x60) / level(sizeHex);
}
