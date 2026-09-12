#pragma once

// ───────────────────────────────────────────────────────────────────────────
// The reverb's PRESETS — four names for four sets of cell values, and nothing else.
//
// ⚠️⚠️ **THE NAME IS NOT STORED IN THE PROJECT AND IS NOT AN ARGUMENT TO THE DSP.** A project holds
// SIZE, DAMP, PRE, WIDE, MOD, DCAY and DENS; the TYPE cell writes all seven at once and then reads
// the name back by matching. Nothing downstream can ask "is this HALL?", because after one turn of
// any other cell the question has no answer — which is the point. A preset is a starting place, not a mode. This is
// the shape delay-presets.h already has, and the two are deliberately the same shape.
//
// ⚠️⚠️ **`NORMAL`'S ROW IS THE STRUCT DEFAULT** (model.h): SIZE 60, DAMP 80, PRE 00, WIDE 80,
// MOD 10, DCAY 60, DENS 99 — and a project written before these cells existed loads without them and
// lands on that row, so the row and the struct must be changed TOGETHER or a brand-new project reads
// as USER. It is the one row whose values are not free.
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
// algorithm 0's is eight fixed delay lines: a "PLATE" there could only be a bright hall, and a preset
// list is a promise about what its names mean. ⚠️ **A ROW HAS TO MEAN ITS NAME ON BOTH ALGORITHMS** —
// there is one list, not one per algorithm — so a density control on the second is not enough on its
// own.
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
    // ⚠️ **THE TWO ALGORITHM-1 CELLS ARE IN EVERY ROW EVEN THOUGH ALGORITHM 0 CANNOT HEAR THEM.** A
    // preset that left them alone would name a row while two cells under it said something else, and
    // the match below would then answer with a name the sound does not have. They are written on both
    // algorithms and simply do nothing on the first.
    int         decay;    // 00-FF, how fast the tank empties — MVERB only
    int         density;  // 00-FF, how fast the input smears — MVERB only
};

// ⚠️ **THE WANDER RISES WITH THE TAIL AND ONLY WITH IT.** A short tail is over before movement in it
// could be heard as anything but detuning, so ROOM has none at all; a 12-second one sits still and
// turns metallic without some, so CAVE has the most. It is the length that decides how much a row
// can carry, not how grand the name sounds.
//
// ⚠️⚠️ **THE LAST TWO COLUMNS ARE NOT ALGORITHM 0'S TIMES RE-TYPED.** MVerb's own decay is a separate
// quantity from its room, and at a row's SIZE the DCAY cell spans a range of its own: measured on the
// module, NORMAL's room reaches 0.55 s at DCAY 00 and 4.06 s at FE, CAVE's 0.99 s to 6.51 s. So the
// rows are voiced for the RATIO the name promises rather than for a number the second algorithm
// cannot reach — ROOM about a third of NORMAL, HALL about twice it, CAVE about five times, where
// algorithm 0 spends 0.66 s to 10 s on the same four names. **NORMAL's pair is pinned to the struct
// default and is the one that may not move on its own.**
inline constexpr ReverbPreset kReverbPresets[] = {
    // 1.9 s on algorithm 0, 1.1 s on algorithm 1, and bright enough to sit under anything.
    {"NORMAL", 0x60, 0x80, 0x00, 0x80, 0x10, 0x60, 0x99},
    // Tight, bright and close in: 0.6 s, almost no gap before it, a narrower image than the others
    // because a small room does not arrive from the sides, and a tail held perfectly still. The
    // smallest smear of the four as well — a small room stays defined, where a wash reads as a bigger
    // space than the walls are.
    {"ROOM",   0x18, 0xA0, 0x04, 0x70, 0x00, 0x30, 0x66},
    // The gap is what makes it a hall — 30 ms of clear air before the tail, so the source stays in
    // front of it — and 3.6 s of darker, wider tail behind that.
    {"HALL",   0x88, 0x60, 0x33, 0xC0, 0x20, 0x90, 0xC0},
    // 12 s, the darkest of the four, with the widest image and the deepest wander. Its DCAY is the
    // longest a row goes: FF is the freeze, and a preset is a starting place, not a held chord.
    {"CAVE",   0xD0, 0x20, 0x55, 0xFF, 0x40, 0xF0, 0xE6},
};
inline constexpr int kReverbPresetCount = 4;

// The index shown on the TYPE cell when no preset matches. It is one past the last, so the cell's
// range is 0..kReverbPresetCount while the cells are hand-set and 0..kReverbPresetCount-1 once a
// preset is chosen — which is what makes USER reachable to LEAVE and impossible to ENTER on purpose.
inline constexpr int kReverbPresetUser = kReverbPresetCount;

/**
 * Which preset these cells are, or `kReverbPresetUser` when they are nobody's.
 *
 * ⚠️ DCAY and DENS are matched on BOTH algorithms even though only one can hear them — the row wrote
 * them, so the row has to own them. Turning DCAY on MVERB drops TYPE to USER, which is exactly what
 * turning any other cell does; the alternative would be a screen reading HALL after the tail's length
 * had been changed underneath it.
 */
inline int reverb_preset_match(int size, int damp, int pre, int width, int mod, int decay,
                               int density) {
    for (int i = 0; i < kReverbPresetCount; ++i) {
        const ReverbPreset& r = kReverbPresets[i];
        if (r.size == size && r.damp == damp && r.pre == pre && r.width == width && r.mod == mod &&
            r.decay == decay && r.density == density)
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

// ───────────────────────────────────────────────────────────────────────────
// The ALGORITHM cell — WHICH reverb is sounding, not how it is set.
//
// ⚠️⚠️ **A MODE'S NUMBER IS ITS IDENTITY: APPEND, NEVER INSERT**, exactly as with `EFFECT_TYPES` and
// `SettingsRow`. **0 is the reverb that shipped in 0.9.8** — a project that never writes the field
// loads on it, and it must sound bit-identical to what was approved by ear on 2026-09-10.
//
// ⚠️⚠️ **THE SHARED CELLS DO NOT CROSS.** SIZE is a decay time to algorithm 0 and a room's dimensions
// to algorithm 1; DAMP is a corner in Hz to one and a normalised amount to the other; MOD is a pitch
// wander to one and an early/late balance to the other. Each algorithm owns its own reading, which is
// why every mapping in this file is a named function rather than arithmetic inline in `setParams`.
// **Switching the cell changes what the cells MEAN, and nothing rewrites them** — that is deliberate:
// a project keeps the numbers the user typed.
//
// ⚠️ **AND TWO CELLS ARE NOT SHARED AT ALL.** DCAY and DENS have no counterpart in algorithm 0, so the
// EFFECTS screen HIDES them while it is chosen (`effects_row_layout.h`). They are still stored and
// still serialized, so switching away and back does not lose them.
//
// ⚠️ **PRE AND WIDE ARE THE EXCEPTION AND MEAN THE SAME THING IN BOTH.** They are applied OUTSIDE the
// algorithm — the pre-delay ring and the mid/side pair both live in ReverbModule — so there is one
// implementation and one meaning whichever algorithm is sounding. `mverb.h`'s own pre-delay line was
// removed for exactly this reason.
inline constexpr int kReverbAlgoCount = 2;

// OLD:   the Schroeder-Moorer wash that shipped — eight modulated lines, no early reflections.
// MVERB: a Dattorro tank with an eight-tap early-reflection bank per channel, so the walls arrive
//        before the tail does.
inline constexpr const char* kReverbAlgoNames[kReverbAlgoCount] = {"OLD", "MVERB"};

/** The name for the ALGO cell, clamped — an unknown number reads as the shipping reverb. */
inline const char* reverb_algo_name(int algo) {
    return kReverbAlgoNames[(algo >= 0 && algo < kReverbAlgoCount) ? algo : 0];
}

// ───────────────────────────────────────────────────────────────────────────
// ALGORITHM 1 (MVERB) — its reading of the five shared cells, plus the two only it has.
//
// ⭐ **NONE OF THESE NEEDS A LEVEL COMPENSATION, AND THAT IS THE POINT OF THE TOPOLOGY.** Algorithm
// 0's output is built only out of its delay lines' state, so its SIZE was also a 19 dB volume control
// and `reverb_size_gain` exists to divide that back out. MVerb's output is a fixed set of taps INSIDE
// the tank, so measured on steady noise its level moves 1.7 dB across the whole of SIZE and 3.5 dB
// across the whole of MOD. There is nothing to divide out.

/** SIZE 00-FF → MVerb's room dimensions, the whole of 0..1. */
inline float mverb_size_param(int sizeHex) { return sizeHex / 255.0f; }

/**
 * DCAY 00-FF → how fast the tank empties, INDEPENDENT of the room's size.
 *
 * ⚠️ **THIS CURVE WAS FITTED WHILE SIZE STILL DROVE IT**, which is why it is linear and still lands on
 * a near-geometric tail: measured against SIZE moving with it, the pair walked 0.23 s to 8.3 s. Split
 * apart, DCAY alone spans a similar range at any one room size and SIZE re-colours it.
 *
 * ⚠️⚠️ **FF IS A FREEZE AND SITS JUST UNDER A CLIFF.** The tank's loop gain is `0.7995·decay + 0.005`,
 * so it reaches 1 — where the tail stops decaying and starts GROWING — at a decay of 1.2445. Measured:
 * 1.2435 still decays, 1.25 runs away without bound. 1.24 is the value here because the margin is what
 * makes it safe, not because the tail is audibly different. FF is the one value not on the curve, the
 * same promise algorithm 0's FF makes.
 */
inline float mverb_decay_param(int decayHex) {
    if (decayHex >= 0xFF) return 1.24f;
    return 0.15f + 0.85f * (decayHex / 255.0f);
}

/**
 * DENS 00-FF → how fast the input smears before it reaches the tank — the feedback of the four series
 * allpasses ahead of it.
 *
 * ⚠️ The whole of 00–FF is used and the algorithm scales it to a loop gain of 0.005–0.80 itself, so
 * unlike SIZE and DAMP there is nothing to linearise here: the cell IS the quantity.
 */
inline float mverb_density_param(int densHex) { return densHex / 255.0f; }

/**
 * DAMP 00-FF → MVerb's damping. ⚠️ **IT IS THE SAME CORNER IN Hz THE OTHER ALGORITHM USES** — this
 * routes through `reverb_damp_freq` rather than scaling the cell, so DAMP is the one voicing cell
 * whose meaning genuinely survives the crossing. MVerb's filter tops out at 18.5 kHz against
 * ReverbSc's 20 kHz, which is why the top clamps; nothing is audible up there in a reverb tail.
 */
inline float mverb_damp_param(int dampHex) {
    float hz = reverb_damp_freq(dampHex);
    if (hz > 18500.0f) hz = 18500.0f;
    return 1.0f - (hz - 100.0f) / 18400.0f;
}

/**
 * MOD 00-FF → the EARLY REFLECTIONS' share of the output.
 *
 * ⚠️⚠️ **MVerb's `EARLYMIX` IS THE LATE TAIL'S SHARE, SO THIS RUNS BACKWARDS**: the cell rises as the
 * parameter falls. 00 is the tail alone, which is what the other algorithm can only ever be, and FF
 * is 70 % walls. ⚠️ The last 30 % is deliberately out of reach: with no tail behind them the taps are
 * a slap-back rather than a reverb, and the echo already does that better.
 *
 * ⚠️ MOD's cell means a PITCH WANDER to algorithm 0 and this to algorithm 1, and the default 0x10 is
 * neither's neutral by accident — it is algorithm 0's shipped wander, and it lands here as almost no
 * early reflections at all, so a project switched across arrives at the tail it already had.
 */
inline float mverb_early_param(int modHex) { return 1.0f - 0.7f * (modHex / 255.0f); }

/**
 * The input bandwidth, wide open. The reverb already has an INP EQ row for shaping what reaches it,
 * and a second brightness control that was not a cell would only make DAMP ambiguous.
 */
inline constexpr float kMverbBandwidth = 0.9f;

/**
 * ⚠️⚠️ **THE TWO ALGORITHMS ARE NOT THE SAME LOUDNESS, AND THIS IS WHAT MAKES THEM ONE.** Measured on
 * steady noise at the default cells, MVerb ran 5.97 dB above the shipping reverb — enough that
 * switching the cell would read as "the new one is better" when it was only louder, and enough that
 * the REV fader would have to be re-set on every switch. Anchored at SIZE 60 / DAMP 80, which is the
 * struct default and NORMAL's row.
 */
inline constexpr float kMverbOutputTrim = 0.5032f;
