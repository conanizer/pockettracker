#ifndef POCKETTRACKER_SONGCORE_AUTOMATION_H
#define POCKETTRACKER_SONGCORE_AUTOMATION_H

// ─── AUS / AUF — what can be automated, and how a pair is found ──────────────────────────────────
//
// AUS beside a parameter starts a ramp from that value with a curve; an AUF on a later step sets
// the value it arrives at:
//
//     step 00   VOL 00  AUS 80        ← start at 0, linear
//     step 08   AUF FF                ← arrive at 255 eight steps later
//
// This header is the pure half: which parameters can ramp, and how AUS/AUF pair up, in STEP indices.
// The scheduler turns a span into events.
// ⚠️ Keep pairing in step space. A pre-scan for frame positions would double-advance the groove
// step; the emitter already knows each step's real duration as it walks.

#include <cstdint>
#include <vector>

#include "automation_curve.h"   // the shape, the byte, the one EQ-morph rule — shared with the engine
#include "effects.h"
#include "event.h"
#include "model.h"

namespace songcore {

// ─── The registry — which parameters a ramp can move ─────────────────────────────────────────────
//
// A ramp is the parameter's own CC, emitted more often. To add one: give it a CC id, an arm in
// `EngineConsumer::consume`, a queued apply on the audio thread, and a row here.
//
// What the endpoints mean is the one thing that differs per row:
//  • `BYTE` — they are values, and the ramp interpolates the byte.
//  • `EQ_PRESET` — they are indices into `Project::eqPresets`; interpolating the index would step
//    through unrelated presets, so the twelve band numbers the two presets hold are what moves
//    (`ExtEqMorphPayload`).
enum class RampKind { BYTE, EQ_PRESET };

struct AutomatableParam {
    int      fxCode;  // the effect the author types, and where AUS reads the ramp's start value
    uint8_t  ccId;    // BYTE: the EV_CC id the ramp emits — the SAME one the per-step effect emits
    bool     global;  // the record rides TRACK_GLOBAL rather than the track's own lane
    RampKind kind;
};

inline constexpr AutomatableParam AUTOMATABLE_PARAMS[] = {
    { FX_VOLUME, CC_VOLUME,      false, RampKind::BYTE },  // Vxx — the phraseVol channel
    { FX_PAN,    CC_PAN,         false, RampKind::BYTE },
    { FX_RSEND,  CC_REVERB_SEND, false, RampKind::BYTE },
    { FX_DSEND,  CC_DELAY_SEND,  false, RampKind::BYTE },
    { FX_VTR,    CC_TRACK_VOL,   false, RampKind::BYTE },
    // Per-note sweeps: they die with the note, so nothing to restore on stop().
    { FX_CUT,    CC_FILTER_CUT,  false, RampKind::BYTE },
    { FX_RES,    CC_FILTER_RES,  false, RampKind::BYTE },
    // LPF / HPF / BPF ramp the cutoff and re-assert the type on every tick.
    { FX_LPF,    CC_FILTER_LP,   false, RampKind::BYTE },
    { FX_HPF,    CC_FILTER_HP,   false, RampKind::BYTE },
    { FX_BPF,    CC_FILTER_BP,   false, RampKind::BYTE },
    // ⚠️ CRU is deliberately absent and would pass the assert below: its two nibbles are separate
    // numbers, and interpolating the byte wraps the right one sixteen times.
    { FX_DRV,    CC_DRIVE,       false, RampKind::BYTE },
    // FIN ramps as a pitch bend: it retunes the sounding note (effects.h).
    { FX_FIN,    CC_FINE_TUNE,   false, RampKind::BYTE },
    // ⚠️ Global: on a track's own lane, the EXTERNAL-routing gate would swallow the master fade on
    // any track playing a MIDI instrument (event.h).
    { FX_VMV,    CC_MASTER_VOL,  true,  RampKind::BYTE },
    // TIM: the delay head glides and shifts pitch — the ramp is the point of the command. Global,
    // with the same restore-on-stop debt as VMV.
    { FX_TIM,    CC_DELAY_TIME,  true,  RampKind::BYTE },
    // ── The EQ presets: these emit a band set, not a controller value ─────────────────────────────
    //
    // ⚠️ EQN is per VOICE: a note-on resets the voice's EQ from its instrument, so the morph re-asserts
    // one tic after each retrigger and does nothing over a silent track. EQM is the global sweep.
    { FX_EQN,    0,              false, RampKind::EQ_PRESET },
    // ⚠️ EQM replaces the master EQ and holds, so the host restores it on stop(). The scheduler sets
    // `eqmActive_` from the ramp as well as from the per-step effect.
    { FX_EQM,    0,              true,  RampKind::EQ_PRESET },
};

inline constexpr int AUTOMATABLE_PARAM_COUNT =
    static_cast<int>(sizeof(AUTOMATABLE_PARAMS) / sizeof(AutomatableParam));

/** The registry row for an effect code, or nullptr when that effect cannot be automated. */
inline constexpr const AutomatableParam* automatable_param(int fxCode) {
    for (int i = 0; i < AUTOMATABLE_PARAM_COUNT; ++i)
        if (AUTOMATABLE_PARAMS[i].fxCode == fxCode) return &AUTOMATABLE_PARAMS[i];
    return nullptr;
}

// A BYTE ramp interpolates 0-255, so a parameter whose cell caps below 0xFF could be ramped to a
// value its own cell cannot hold.
inline constexpr bool automatable_params_are_full_range() {
    for (int i = 0; i < AUTOMATABLE_PARAM_COUNT; ++i)
        if (AUTOMATABLE_PARAMS[i].kind == RampKind::BYTE &&
            effect_value_max(AUTOMATABLE_PARAMS[i].fxCode) != 255) return false;
    return true;
}
static_assert(automatable_params_are_full_range(),
              "an automatable BYTE parameter must accept the full 00-FF byte — the ramp interpolates "
              "in that domain and would emit values its own cell cannot hold");

// An EQ_PRESET endpoint is a slot index; this is why pairing needs no runtime range check.
inline constexpr bool automatable_preset_params_are_slot_range() {
    for (int i = 0; i < AUTOMATABLE_PARAM_COUNT; ++i)
        if (AUTOMATABLE_PARAMS[i].kind == RampKind::EQ_PRESET &&
            effect_value_max(AUTOMATABLE_PARAMS[i].fxCode) != POOL_EQPRESETS - 1) return false;
    return true;
}
static_assert(automatable_preset_params_are_slot_range(),
              "an EQ_PRESET parameter's cell must cap at the last preset slot — its endpoints are "
              "indices into Project::eqPresets, and the pairing has no other range check");

// ─── The EQ morph — what an EQ_PRESET ramp holds at position `t` ──────────────────────────────────
//
// The per-band rule is `automation_eq_band_at` (shared with the table path); this is only the lookup.
// Slots are clamped because a hand-edited project file can hold an out-of-range one.
inline ExtEqMorphPayload eq_morph_at(const Project& project, int startSlot, int destSlot,
                                     int curveByte, double t) {
    ExtEqMorphPayload m{};
    const int last = static_cast<int>(project.eqPresets.size()) - 1;
    if (last < 0) return m;
    auto slot = [last](int s) { return static_cast<size_t>(s < 0 ? 0 : (s > last ? last : s)); };
    const EqPreset& from = project.eqPresets[slot(startSlot)];
    const EqPreset& to   = project.eqPresets[slot(destSlot)];

    for (int i = 0; i < 3; ++i) {
        // A file with fewer than three bands leaves the rest zeroed, which reads as OFF.
        if (i >= static_cast<int>(from.bands.size()) || i >= static_cast<int>(to.bands.size())) break;
        const EqBand& a = from.bands[static_cast<size_t>(i)];
        const EqBand& b = to.bands[static_cast<size_t>(i)];
        const AutomationEqBand v = automation_eq_band_at({ a.type, a.freq, a.gain, a.q },
                                                         { b.type, b.freq, b.gain, b.q },
                                                         curveByte, t);
        m.type[i] = static_cast<uint8_t>(v.type);
        m.freq[i] = static_cast<uint8_t>(v.freq);
        m.gain[i] = static_cast<uint8_t>(v.gain);
        m.q[i]    = static_cast<uint8_t>(v.q);
    }
    return m;
}

/** Two morph ticks that would set the engine to the same thing — the de-dup test. */
inline bool eq_morph_equal(const ExtEqMorphPayload& a, const ExtEqMorphPayload& b) {
    for (int i = 0; i < 3; ++i)
        if (a.type[i] != b.type[i] || a.freq[i] != b.freq[i] ||
            a.gain[i] != b.gain[i] || a.q[i]    != b.q[i]) return false;
    return true;
}

/** Is this byte a slot an EQ_PRESET ramp may use as an endpoint? */
inline constexpr bool is_eq_preset_slot(int value) {
    return value >= 0 && value < POOL_EQPRESETS;
}

// ─── The pairing ─────────────────────────────────────────────────────────────────────────────────

/**
 * One ramp, as seen from ONE phrase of the walk.
 *
 * `span` and `stepOffset` describe the whole ramp (what the curve is evaluated against);
 * `ausStep`/`aufStep` and the slots are about this phrase and are −1/0 when that end is elsewhere.
 */
struct RampSpec {
    int      fxCode   = FX_NONE;
    uint8_t  ccId     = 0;
    bool     global   = false;
    // BYTE: values. EQ_PRESET: `Project::eqPresets` indices.
    RampKind kind     = RampKind::BYTE;
    int     ausStep   = -1;   // the step carrying AUS, or −1: the ramp opened in an earlier phrase
    int     aufStep   = -1;   // the step carrying the AUF, or −1: it arrives in a later phrase
    int     paramSlot = 0;    // 1-3: the slot AUS read its start value from
    int     ausSlot   = 0;    // 1-3: where AUS itself sits
    int     aufSlot   = 0;    // 1-3: where the AUF that closed it sits
    int     startByte = 0;
    int     destByte  = 0;
    int     curveByte = AUS_CURVE_LINEAR;

    int     span       = 0;   // steps over the whole ramp; never zero for a paired ramp
    // Steps elapsed since the AUS = stepOffset + stepIndex. Negative (−ausStep) in the AUS's own
    // phrase, positive once the ramp has crossed into a later one.
    int     stepOffset = 0;

    // The AUS cell's position in the chain walk (or the phrase, with no chain) and its slot. A CHA
    // can eat the AUS, and every later phrase of the span needs to know; the scheduler keys on these.
    int     originAbs  = -1;
    int     originSlot = 0;
};

/** Does a note-on reset this parameter to the instrument's value? Then every note inside the fade,
 *  ARP/RPT retriggers included, must be handed the fade's value. */
inline bool ramp_moves_voice(const RampSpec& r) {
    return !r.global && r.ccId != CC_TRACK_VOL;
}

/** …and of those, the two a note-on carries in its own fields rather than as a controller. */
inline bool ramp_rides_note_on(const RampSpec& r) {
    return r.ccId == CC_VOLUME || r.ccId == CC_PAN;
}

/** Does this phrase carry an AUS or AUF cell at all? A cheap skip — never a substitute for pairing. */
inline bool phrase_has_ramp_cell(const Phrase& phrase) {
    for (const PhraseStep& step : phrase.steps) {
        for (int slot = 1; slot <= 3; ++slot) {
            const int type = step_fx_type(step, slot);
            if (type == FX_AUS || type == FX_AUF) return true;
        }
    }
    return false;
}

/**
 * Every ramp `phrase` declares, in the order they open, walking from `startRow`.
 *
 *  • AUS looks LEFT on its own step for the nearest automatable effect; its value is the start.
 *    With nothing automatable to its left the AUS is inert and leaves any open ramp alone.
 *  • The first AUF on a LATER step closes. One on the AUS's own step is inert — slot order is not
 *    time order.
 *  • An EQ_PRESET endpoint that is not a slot is inert at either end; the AUS stays open, so a later
 *    legal AUF still closes it. Unused AUS/AUF cells draw dim.
 *  • The last AUS wins; ramps do not nest.
 *  • An AUS still open at the end of the phrase produces nothing.
 *
 * Reads the AUTHORED step, before CHA/RND/RNL, so a ramp is always reproducible.
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
                    const int startValue = step_fx_value(step, left);
                    // The nearest automatable effect is the parameter, so a bad endpoint ends the search.
                    if (p->kind == RampKind::EQ_PRESET && !is_eq_preset_slot(startValue)) break;
                    open           = RampSpec{};
                    open.fxCode    = p->fxCode;
                    open.ccId      = p->ccId;
                    open.global    = p->global;
                    open.kind      = p->kind;
                    open.ausStep   = stepIndex;
                    open.paramSlot = left;
                    open.ausSlot   = slot;
                    open.startByte = startValue;
                    open.curveByte = step_fx_value(step, slot);
                    isOpen         = true;
                    break;
                }
            } else if (type == FX_AUF && isOpen && stepIndex > open.ausStep) {
                // ⚠️ Before the close: an endpoint that is not a slot leaves the AUS open and this
                // cell unused (so `RampCells` dims it).
                if (open.kind == RampKind::EQ_PRESET && !is_eq_preset_slot(step_fx_value(step, slot)))
                    continue;
                open.aufStep    = stepIndex;
                open.aufSlot    = slot;
                open.destByte   = step_fx_value(step, slot);
                open.span       = open.aufStep - open.ausStep;
                open.stepOffset = -open.ausStep;   // see RampSpec: elapsed = stepOffset + stepIndex
                open.originAbs  = open.ausStep;
                open.originSlot = open.ausSlot;
                out.push_back(open);
                isOpen = false;
            }
        }
    }
    return out;
}

// ─── Pairing across a chain ──────────────────────────────────────────────────────────────────────
//
// The AUF may sit in a later phrase of the SAME chain, so a fade can span up to 256 steps.
// ⚠️ The chain is the boundary: a span past it would have to survive song moves, a chain played from
// two rows, and CHAIN-mode wraps.
// ⚠️ Re-derived from (chain, chainRow) on every call — never carried in `TrackState`. A live edit
// rolls the lookahead back without rewinding `TrackState`, a chain can be re-entered, and a phrase
// can be scheduled twice; carried state would be wrong in all three.

/**
 * Every ramp playing over the phrase at `chainRow`, whether it opened there or earlier.
 *
 * `startRow` (the HOP entry) applies only to ramps opening in THIS phrase. Empty chain rows add no
 * steps, matching the traversal.
 * ⚠️ The walk counts authored steps. A HOP inside the span plays fewer, so the middle of the fade
 * runs slightly ahead; both endpoints stay exact. The real walk is unknowable in advance (the HOP can
 * be CHA-gated).
 */
inline std::vector<RampSpec> find_ramps_in_chain(const Project& project, const Chain& chain,
                                                 int chainRow, int startRow = 0) {
    std::vector<RampSpec> out;
    if (chainRow < 0 || chainRow >= 16) return out;

    // Absolute step of each row's step 0; −1 for a row that never plays.
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

    // Same rules as `find_ramps`, over the whole chain in absolute steps.
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
                    // A HOP into this phrase below the AUS skips it.
                    if (row == chainRow && stepIndex < startRow) continue;
                    for (int left = slot - 1; left >= 1; --left) {
                        const AutomatableParam* p = automatable_param(step_fx_type(step, left));
                        if (p == nullptr) continue;
                        const int startValue = step_fx_value(step, left);
                        if (p->kind == RampKind::EQ_PRESET && !is_eq_preset_slot(startValue)) break;
                        open           = RampSpec{};
                        open.fxCode    = p->fxCode;
                        open.ccId      = p->ccId;
                        open.global    = p->global;
                        open.kind      = p->kind;
                        open.paramSlot = left;
                        open.ausSlot   = slot;
                        open.startByte = startValue;
                        open.curveByte = step_fx_value(step, slot);
                        openAbs        = absStep;
                        openRow        = row;
                        isOpen         = true;
                        break;
                    }
                } else if (type == FX_AUF && isOpen && absStep > openAbs) {
                    // ⚠️ Must stay above `isOpen = false`: written below it, a bad endpoint would
                    // close the ramp and emit nothing.
                    if (open.kind == RampKind::EQ_PRESET &&
                        !is_eq_preset_slot(step_fx_value(step, slot))) continue;
                    const int aufAbs = absStep;
                    isOpen = false;
                    // Only spans overlapping this phrase are returned.
                    if (aufAbs < hereAbs || openAbs > hereEnd) break;
                    RampSpec r   = open;
                    r.destByte   = step_fx_value(step, slot);
                    r.span       = aufAbs - openAbs;
                    r.stepOffset = hereAbs - openAbs;
                    r.ausStep    = (openRow == chainRow) ? openAbs - hereAbs : -1;
                    r.aufStep    = (row == chainRow)     ? aufAbs  - hereAbs : -1;
                    r.aufSlot    = (row == chainRow)     ? slot              : 0;
                    r.originAbs  = openAbs;
                    r.originSlot = open.ausSlot;
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
// A phrase can play in many chain contexts, and a ramp belongs to a chain walk. A cell draws live if
// ANY context uses it — dimming means "inert", and a cell live somewhere is not.

/** The AUS/AUF cells of one phrase that a ramp uses, `[step][slot]` with slots 1-3. Ends in another
 *  phrase are −1 in the spec and skipped, so marking from several rows is a union. */
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

    /** Does this FX cell take part in a ramp? Anything but AUS/AUF is always active. */
    bool active(int fxType, int stepIndex, int slot) const {
        if (fxType != FX_AUS && fxType != FX_AUF) return true;
        if (stepIndex < 0 || stepIndex >= 16 || slot < 1 || slot > 3) return false;
        return used[stepIndex][slot];
    }
};

/**
 * Every AUS/AUF cell of phrase `phraseId` that a ramp uses, over every chain row that plays it. A
 * phrase no chain references pairs within itself.
 * Asks `find_ramps_in_chain`, so the editor cannot show a fade the emitter will not play.
 */
inline RampCells find_ramp_cells(const Project& project, int phraseId) {
    RampCells cells;
    if (phraseId < 0 || phraseId >= static_cast<int>(project.phrases.size())) return cells;

    // A phrase with no AUS/AUF cell of its own can have no marked cell — exact, and it keeps the
    // chain walk off the 60 Hz redraw of an ordinary phrase.
    const Phrase& self = project.phrases[static_cast<size_t>(phraseId)];
    if (!phrase_has_ramp_cell(self)) return cells;

    bool placed = false;
    for (const Chain& chain : project.chains) {
        for (int row = 0; row < CHAIN_ROWS; ++row) {
            if (chain_phrase_ref(chain, row) != phraseId) continue;
            placed = true;
            cells.mark(find_ramps_in_chain(project, chain, row));
        }
    }
    if (!placed) cells.mark(find_ramps(self));
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
