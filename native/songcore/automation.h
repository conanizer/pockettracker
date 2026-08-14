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
    // The instrument filter. A ramp on either is a per-note sweep and dies with the note that carries
    // it — nothing to restore on stop(), unlike the two faders below.
    { FX_CUT,    CC_FILTER_CUT,  false },
    { FX_RES,    CC_FILTER_RES,  false },
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

/**
 * One declared ramp, as seen from ONE phrase of the walk.
 *
 * A span may cross phrase boundaries (`find_ramps_in_chain`), so the fields divide into two kinds:
 * `span`/`stepsBefore` describe the WHOLE ramp and are what the curve is evaluated against, while
 * `ausStep`/`aufStep` and the slots are about THIS phrase and go to −1 when the corresponding end is
 * in a different one. A ramp confined to a single phrase has `stepsBefore == 0` and
 * `span == aufStep − ausStep`, which is what `find_ramps` produces on its own.
 */
struct RampSpec {
    int     fxCode    = FX_NONE;
    uint8_t ccId      = 0;
    bool    global    = false;
    int     ausStep   = -1;   // the step carrying AUS, or −1: the ramp opened in an earlier phrase
    int     aufStep   = -1;   // the step carrying the AUF, or −1: it arrives in a later phrase
    int     paramSlot = 0;    // 1-3: the slot AUS read its start value from
    int     ausSlot   = 0;    // 1-3: where AUS itself sits
    int     aufSlot   = 0;    // 1-3: where the AUF that closed it sits
    int     startByte = 0;
    int     destByte  = 0;
    int     curveByte = AUS_CURVE_LINEAR;

    // The span, in steps, over the whole ramp — the denominator `t` is measured against, and never
    // zero for a ramp that paired.
    int     span       = 0;
    // Add this phrase's step index to it for the steps elapsed since the AUS. It is NEGATIVE for the
    // phrase the AUS sits in (exactly −ausStep, so the sum is the familiar `stepIndex − ausStep`) and
    // positive once the ramp has crossed a boundary — one signed number instead of two cases.
    int     stepOffset = 0;
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
                open.aufStep    = stepIndex;
                open.aufSlot    = slot;
                open.destByte   = step_fx_value(step, slot);
                open.span       = open.aufStep - open.ausStep;
                open.stepOffset = -open.ausStep;   // see RampSpec: elapsed = stepOffset + stepIndex
                out.push_back(open);
                isOpen = false;
            }
        }
    }
    return out;
}

// ─── Pairing across a chain ──────────────────────────────────────────────────────────────────────
//
// A phrase is 16 steps, which at any useful tempo is a bar or less — so a fade written the way
// `find_ramps` pairs it can never be longer than a bar, and "fade this in over the four bars of the
// intro" is not expressible at all. The AUF is allowed to sit in a LATER PHRASE OF THE SAME CHAIN,
// which is what the walk below works out.
//
// ⚠️ **THE CHAIN IS THE BOUNDARY, AND IT IS A REAL ONE.** Pairing stops at the end of the chain: a
// chain is the unit a track repeats and re-enters, and a span that ran past it would have to survive
// the song moving to a different chain, a chain played from two song rows at once, and CHAIN mode's
// wrap back to row 0. Sixteen rows of sixteen steps is 256 steps of ramp, which is the length of the
// thing anyone has asked for.
//
// ⭐⭐ **IT IS RE-DERIVED ON EVERY CALL, NOT CARRIED IN `TrackState`.** The obvious implementation
// leaves the open AUS in the track's state and lets successive `schedulePhrase` calls advance it, and
// it is wrong in three ways at once, all of them invisible until they are not: `notify_data_changed`
// rolls the lookahead back to an earlier chain row WITHOUT rewinding `TrackState` (see the checkpoint
// ring), so a live edit would jump the fade forward by a phrase; a chain re-entered from a later song
// row would inherit the previous one's open ramp; and the same phrase scheduled twice would advance it
// twice. Deriving the whole picture from (chain, chainRow) each time makes every one of those
// questions unaskable — the same reason the emitter measures `t` in steps rather than accumulating
// frames.

/**
 * Every ramp playing over the phrase at `chainRow`, whether it opened there or in an earlier row.
 *
 * Spans are returned as this phrase sees them: `ausStep` is −1 when the AUS is behind us, `aufStep`
 * is −1 when the AUF is ahead, and `stepsBefore` says how far along the ramp already was when this
 * phrase began. `startRow` is the HOP entry point and applies only to ramps opening in THIS phrase —
 * a phrase entered below its own AUS runs no ramp, exactly as `find_ramps` has it, while one entered
 * below a step a ramp from an earlier row is merely passing through is unaffected.
 *
 * ⚠️ Empty chain rows contribute NO steps, matching the traversal: the scheduler skips them without
 * scheduling anything, so counting them would drift the fade against the notes.
 *
 * ⚠️ **THE WALK IS THE AUTHORED ONE, AND A HOP CAN MAKE THE REAL ONE SHORTER.** A HOP inside the span
 * ends its phrase early, so fewer steps play than this counted and the fade is a little further along
 * than the step count says when the next phrase picks it up. It still starts at the start byte and
 * still arrives at the destination on the AUF's own step — the error is in the middle and bounded by
 * the steps the HOP skipped. Reading the real traversal instead is not available: the HOP itself can
 * be CHA-gated, so how far the phrase gets is not knowable until it has been walked, which is the
 * same reason nothing is ever emitted ahead of the walk.
 */
inline std::vector<RampSpec> find_ramps_in_chain(const Project& project, const Chain& chain,
                                                 int chainRow, int startRow = 0) {
    std::vector<RampSpec> out;
    if (chainRow < 0 || chainRow >= 16) return out;

    // Absolute step of each row's step 0, in the chain's own walk. −1 marks a row that is never
    // played and therefore has no position at all.
    int rowAbs[CHAIN_ROWS];
    int walked = 0;
    for (int row = 0; row < CHAIN_ROWS; ++row) {
        const bool played = chain_phrase_ref(chain, row) >= 0;
        rowAbs[row] = played ? walked : -1;
        if (played) walked += PHRASE_ROWS;
    }
    if (rowAbs[chainRow] < 0) return out;

    const int hereAbs = rowAbs[chainRow];
    const int hereEnd = hereAbs + 15;

    // One pass over the whole chain, in playing order, pairing exactly as `find_ramps` does within a
    // phrase — the rules do not change, only how far the walk can see. Absolute step indices keep the
    // two ends comparable across rows.
    RampSpec open;
    bool     isOpen    = false;
    int      openAbs   = 0;   // absolute step the open AUS sits on
    int      openRow   = -1;  // and the row, so a HOP entry can be applied to the right phrase

    for (int row = 0; row < CHAIN_ROWS; ++row) {
        if (rowAbs[row] < 0) continue;
        const Phrase& phrase = project.phrases[static_cast<size_t>(chain_phrase_ref(chain, row))];
        const int steps = static_cast<int>(phrase.steps.size());
        for (int stepIndex = 0; stepIndex < steps && stepIndex < 16; ++stepIndex) {
            const PhraseStep& step = phrase.steps[static_cast<size_t>(stepIndex)];
            const int absStep = rowAbs[row] + stepIndex;
            for (int slot = 1; slot <= 3; ++slot) {
                const int type = step_fx_type(step, slot);

                if (type == FX_AUS) {
                    // A HOP into this phrase below the AUS means the step never plays, so the ramp is
                    // never declared. Only applies to the phrase actually being entered.
                    if (row == chainRow && stepIndex < startRow) continue;
                    for (int left = slot - 1; left >= 1; --left) {
                        const AutomatableParam* p = automatable_param(step_fx_type(step, left));
                        if (p == nullptr) continue;
                        open           = RampSpec{};
                        open.fxCode    = p->fxCode;
                        open.ccId      = p->ccId;
                        open.global    = p->global;
                        open.paramSlot = left;
                        open.ausSlot   = slot;
                        open.startByte = step_fx_value(step, left);
                        open.curveByte = step_fx_value(step, slot);
                        openAbs        = absStep;
                        openRow        = row;
                        isOpen         = true;
                        break;
                    }
                } else if (type == FX_AUF && isOpen && absStep > openAbs) {
                    const int aufAbs = absStep;
                    isOpen = false;
                    // Only the part of the chain this phrase can see is any of its business.
                    if (aufAbs < hereAbs || openAbs > hereEnd) break;
                    RampSpec r   = open;
                    r.destByte   = step_fx_value(step, slot);
                    r.span       = aufAbs - openAbs;
                    r.stepOffset = hereAbs - openAbs;
                    r.ausStep    = (openRow == chainRow) ? openAbs - hereAbs : -1;
                    r.aufStep    = (row == chainRow)     ? aufAbs  - hereAbs : -1;
                    r.aufSlot    = (row == chainRow)     ? slot              : 0;
                    if (r.ausStep < 0) r.ausSlot = 0;    // the AUS cell is not in this phrase
                    out.push_back(r);
                    break;
                }
            }
        }
    }
    return out;
}

// ─── Which cells of ONE phrase a ramp uses — the question the editor asks ────────────────────────
//
// The editor draws a phrase, and a phrase has no single playing context: the same sixteen steps may
// sit at several rows of a chain, in several chains, in neither. A ramp, meanwhile, is a property of
// a CHAIN WALK. So the cell question — *is this AUS doing anything?* — is answered over EVERY context
// the project plays the phrase in, and the cell is live if any one of them uses it.
//
// ⚠️ **THE UNION IS THE HONEST ANSWER, AND ASKING ONE CONTEXT IS NOT.** Dimming means "you wrote
// something inert"; a cell live in one of its contexts is not inert, and saying so would send the
// author hunting a bug in a fade that plays. The imprecision runs the other way instead — a cell live
// in one chain draws live everywhere, which is the most an editor with one cell and many contexts can
// say without lying.

/**
 * The AUS/AUF cells of one phrase that a ramp really uses, indexed `[step][slot]` with slots 1-3.
 *
 * Filled from `RampSpec`s, whose `ausStep`/`aufStep` are already −1 for an end living in ANOTHER
 * phrase — so marking is safe to do with the specs of any row, and folding several rows together is
 * exactly the union above.
 */
struct RampCells {
    bool used[16][4] = {};

    void mark(const std::vector<RampSpec>& ramps) {
        for (const RampSpec& r : ramps) {
            if (r.ausStep >= 0 && r.ausStep < 16 && r.ausSlot >= 1 && r.ausSlot <= 3)
                used[r.ausStep][r.ausSlot] = true;
            if (r.aufStep >= 0 && r.aufStep < 16 && r.aufSlot >= 1 && r.aufSlot <= 3)
                used[r.aufStep][r.aufSlot] = true;
        }
    }

    /**
     * Does this FX cell take part in a ramp?
     *
     * Only AUS and AUF can answer no — every other effect stands on its own, so a cell carrying one
     * is active by definition and the caller may ask about any slot. The three ways to write an AUS
     * that does nothing (nothing automatable to its left, no AUF after it, an AUF sharing its step)
     * and the two ways to write an inert AUF (no ramp open, or a second AUF after one already closed)
     * all reduce to the same thing: the pairing did not use this cell.
     */
    bool active(int fxType, int stepIndex, int slot) const {
        if (fxType != FX_AUS && fxType != FX_AUF) return true;
        if (stepIndex < 0 || stepIndex >= 16 || slot < 1 || slot > 3) return false;
        return used[stepIndex][slot];
    }
};

/**
 * Every AUS/AUF cell of phrase `phraseId` that a ramp uses, over every chain row that plays it.
 *
 * A phrase no chain references has no walk to be read in, and falls back to pairing within itself —
 * which is what an author writing a phrase before placing it should see.
 *
 * ⚠️ Asks `find_ramps_in_chain` rather than re-deriving the rules, so an editor drawing this cannot
 * claim a fade the emitter will not play — they are reading one answer, not two implementations of
 * it. That is also why the whole phrase is answered in ONE pass and handed to the caller as a mask:
 * per-cell scanning would put a chain walk under every glyph.
 */
inline RampCells find_ramp_cells(const Project& project, int phraseId) {
    RampCells cells;
    if (phraseId < 0 || phraseId >= static_cast<int>(project.phrases.size())) return cells;

    bool placed = false;
    for (const Chain& chain : project.chains) {
        for (int row = 0; row < CHAIN_ROWS; ++row) {
            if (chain_phrase_ref(chain, row) != phraseId) continue;
            placed = true;
            cells.mark(find_ramps_in_chain(project, chain, row));
        }
    }
    if (!placed) cells.mark(find_ramps(project.phrases[static_cast<size_t>(phraseId)]));
    return cells;
}

/** `RampCells::active` for a caller that already holds one context's spans. */
inline bool automation_cell_active(const std::vector<RampSpec>& ramps, int fxType,
                                   int stepIndex, int slot) {
    RampCells cells;
    cells.mark(ramps);
    return cells.active(fxType, stepIndex, slot);
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_AUTOMATION_H
