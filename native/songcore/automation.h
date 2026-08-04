#ifndef POCKETTRACKER_SONGCORE_AUTOMATION_H
#define POCKETTRACKER_SONGCORE_AUTOMATION_H

// ─── AUS / AUF — what can be automated, and how a pair is found ──────────────────────────────────
//
// Automating a parameter means moving it continuously across a span of steps instead of writing it
// once per step. The author puts the parameter at its starting value, puts AUS beside it to say
// "ramp from here, with this curve", and AUF on a later step to say "arrive at this value".
//
//     step 00   VOL 00  AUS 80        ← start at 0, linear
//     step 08   AUF FF                ← arrive at 255 eight steps later
//
// This header is the PURE half: the registry of parameters a ramp can move, the curve, and the
// pairing rule. It emits nothing and knows nothing about frames, ticks, grooves or the transport —
// `find_ramps` answers "which spans does this phrase declare", in STEP indices, and the scheduler
// turns a span into events.
//
// ⚠️ **THAT SPLIT IS LOAD-BEARING, NOT TIDINESS.** Pairing in step space needs no frame table, so the
// groove trap the design doc warned about — a pre-scan that walks the phrase to find span frames and
// double-advances `TrackState::grooveStep` on the way — cannot exist here. The emitter already holds
// each step's real duration as it walks, so a groove-warped span costs it nothing, and a HOP out of
// the phrase truncates the ramp for free by ending the walk.

#include <cstdint>
#include <vector>

#include "effects.h"
#include "event.h"
#include "model.h"

namespace songcore {

// ─── The registry — which parameters a ramp can move ─────────────────────────────────────────────
//
// A ramp is nothing but the parameter's own CC, emitted more often. So the entry price is exactly
// "this parameter has an absolute, sample-accurate set-to-X-at-frame-F path" — a CC id inside the
// existing EV_CC, an arm in `EngineConsumer::consume`, and a queued apply on the audio thread. Those
// three are what live control costs whether or not anything ramps it; automation adds no fourth.
//
// ⚠️ **THE SET IS THIS TABLE, NOT A PROPERTY OF THE FEATURE.** It is short today because only six
// effects have paid that price, not because a ramp is limited to six things. Adding a seventh is:
// give it a CC id, an arm, an apply, and a row here.
//
// ⚠️ There is deliberately no range column and no density column. Every entry's ceiling is already
// `effect_value_max(fxCode)` (asserted below) and the emission grid is one constant for all of them —
// a column holding the same value in every row is a claim that it varies, and the day it stops being
// true the table and the truth part company silently.
struct AutomatableParam {
    int     fxCode;   // the effect the author types, and where AUS reads the ramp's start value
    uint8_t ccId;     // the EV_CC id the ramp emits — the SAME one the per-step effect emits
    bool    global;   // the record rides TRACK_GLOBAL rather than the track's own lane
};

inline constexpr AutomatableParam AUTOMATABLE_PARAMS[] = {
    { FX_VOLUME, CC_VOLUME,      false },  // Vxx — the phraseVol channel
    { FX_PAN,    CC_PAN,         false },
    { FX_RSEND,  CC_REVERB_SEND, false },
    { FX_DSEND,  CC_DELAY_SEND,  false },
    { FX_VTR,    CC_TRACK_VOL,   false },
    // ⚠️ The master fader belongs to no track. Track-scoped, EngineConsumer's external-routing gate
    // would swallow it whenever the carrying track plays an EXTERNAL instrument (event.h) — so the
    // ramp must ride the same TRACK_GLOBAL lane the per-step VMV does, or it dies on exactly the
    // tracks a master fade is most often written on.
    { FX_VMV,    CC_MASTER_VOL,  true  },
};

inline constexpr int AUTOMATABLE_PARAM_COUNT =
    static_cast<int>(sizeof(AUTOMATABLE_PARAMS) / sizeof(AutomatableParam));

/** The registry row for an effect code, or nullptr when that effect cannot be automated. */
inline constexpr const AutomatableParam* automatable_param(int fxCode) {
    for (int i = 0; i < AUTOMATABLE_PARAM_COUNT; ++i)
        if (AUTOMATABLE_PARAMS[i].fxCode == fxCode) return &AUTOMATABLE_PARAMS[i];
    return nullptr;
}

// The ramp interpolates in the authored 0-255 byte domain and emits `byte / 255`, so every value it
// produces is a member of the same 256-value set the per-step effects already emit. A parameter whose
// cell caps below 0xFF would break that: its ramp could type values the cell cannot.
inline constexpr bool automatable_params_are_full_range() {
    for (int i = 0; i < AUTOMATABLE_PARAM_COUNT; ++i)
        if (effect_value_max(AUTOMATABLE_PARAMS[i].fxCode) != 255) return false;
    return true;
}
static_assert(automatable_params_are_full_range(),
              "an automatable parameter must accept the full 00-FF byte — the ramp interpolates in "
              "that domain and would emit values its own cell cannot hold");

// ─── The curve ───────────────────────────────────────────────────────────────────────────────────
//
// AUS's value byte picks the shape, as a continuous family between three anchors. It is a POLYNOMIAL
// on purpose — `+ − ×` only, never `pow` or `exp`: the scheduling TUs compile with no fast-math and
// `-ffp-contract=off`, and the emitted values have to be bit-identical on every platform, which is
// the same reason note→Hz is vendored rather than called (event-schema.md).
constexpr int AUS_CURVE_EASE_IN  = 0x00;  // slow to leave, fast to arrive — cubic up
constexpr int AUS_CURVE_LINEAR   = 0x80;
constexpr int AUS_CURVE_EASE_OUT = 0xFF;  // fast to leave, slow to arrive

/**
 * The eased position, `t` and the result both in [0,1]. 0x80 is exactly `t`; either side blends
 * linearly towards the cubic anchor, so the family is continuous through the middle and the two
 * halves meet at the same value.
 */
inline double automation_shape(int curveByte, double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    if (curveByte <= AUS_CURVE_LINEAR) {
        const double w = curveByte / 128.0;                 // 00 → all ease-in, 80 → all linear
        return (1.0 - w) * (t * t * t) + w * t;
    }
    const double w = (curveByte - AUS_CURVE_LINEAR) / 127.0;  // 80 → all linear, FF → all ease-out
    const double u = 1.0 - t;
    return (1.0 - w) * t + w * (1.0 - u * u * u);
}

/**
 * The byte a ramp holds at position `t`. Rounded half-up — both ends are 0-255 and the shape never
 * leaves [0,1], so the value is never negative and the rounding needs no sign case.
 */
inline int automation_value_byte(int startByte, int destByte, int curveByte, double t) {
    const double v = startByte + (destByte - startByte) * automation_shape(curveByte, t);
    const int    b = static_cast<int>(v + 0.5);
    return b < 0 ? 0 : (b > 255 ? 255 : b);
}

// ─── The pairing ─────────────────────────────────────────────────────────────────────────────────

/** One declared ramp: which parameter, over which steps, from which byte to which, on what curve. */
struct RampSpec {
    int     fxCode    = FX_NONE;
    uint8_t ccId      = 0;
    bool    global    = false;
    int     ausStep   = -1;   // the step carrying AUS — the ramp's first step
    int     aufStep   = -1;   // the step carrying the AUF that closed it
    int     paramSlot = 0;    // 1-3: the slot AUS read its start value from
    int     ausSlot   = 0;    // 1-3: where AUS itself sits
    int     startByte = 0;
    int     destByte  = 0;
    int     curveByte = AUS_CURVE_LINEAR;
};

/**
 * Every ramp `phrase` declares, in the order they open, walking from `startRow`.
 *
 * The rules, and what each one is defending against:
 *
 *  • **AUS looks LEFT on its own step** for the nearest automatable effect, skipping ones that are
 *    not (`VOL 20  PSL 40  AUS 00` ramps the VOL). That effect's type is the parameter and its value
 *    is the start byte, so the number the ramp begins at is a number visible in the grid, typed as an
 *    ordinary effect — and it still emits its own per-step event, so the authored start and the
 *    ramp's first sample agree by construction rather than by arithmetic.
 *  • **An AUS with nothing automatable to its left is INERT** — it does not open, and it does not
 *    close or replace a ramp already open. A curve on its own names no parameter, and guessing one
 *    would move something the author never pointed at.
 *  • **AUF must be on a LATER step**, and the FIRST one closes. An AUF sharing a step with its AUS is
 *    inert and leaves the ramp open: slot order inside a step is not a time order, and the span it
 *    would describe has no duration to interpolate across.
 *  • **The last AUS wins**, matching the last-wins convention the FX slots already follow: a second
 *    AUS before any AUF replaces the open ramp rather than nesting.
 *  • **An unclosed AUS at the end of the phrase produces nothing.** Pairing is per-phrase — the AUF
 *    of a future phrase has not been scheduled yet, and inventing an end would mean guessing it.
 *
 * ⚠️ Reads the AUTHORED step, before CHA/RND/RNL touch anything. A ramp therefore never joins the
 * non-reproducible set (`tools/ptnondet`): randomising the curve or the destination would make the
 * same project render differently twice, and a fade is not a thing anyone wants dice on. RND on an
 * AUS, AUF or start-value slot is ignored for pairing purposes.
 */
inline std::vector<RampSpec> find_ramps(const Phrase& phrase, int startRow = 0) {
    std::vector<RampSpec> out;
    RampSpec              open;
    bool                  isOpen = false;

    const int steps = static_cast<int>(phrase.steps.size());
    const int first = startRow < 0 ? 0 : (startRow > steps ? steps : startRow);

    for (int stepIndex = first; stepIndex < steps; ++stepIndex) {
        const PhraseStep& step = phrase.steps[stepIndex];
        for (int slot = 1; slot <= 3; ++slot) {
            const int type = step_fx_type(step, slot);

            if (type == FX_AUS) {
                for (int left = slot - 1; left >= 1; --left) {
                    const AutomatableParam* p = automatable_param(step_fx_type(step, left));
                    if (p == nullptr) continue;
                    open           = RampSpec{};
                    open.fxCode    = p->fxCode;
                    open.ccId      = p->ccId;
                    open.global    = p->global;
                    open.ausStep   = stepIndex;
                    open.paramSlot = left;
                    open.ausSlot   = slot;
                    open.startByte = step_fx_value(step, left);
                    open.curveByte = step_fx_value(step, slot);
                    isOpen         = true;
                    break;
                }
            } else if (type == FX_AUF && isOpen && stepIndex > open.ausStep) {
                open.aufStep  = stepIndex;
                open.destByte = step_fx_value(step, slot);
                out.push_back(open);
                isOpen = false;
            }
        }
    }
    return out;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_AUTOMATION_H
