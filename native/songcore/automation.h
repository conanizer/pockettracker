#ifndef POCKETTRACKER_SONGCORE_AUTOMATION_H
#define POCKETTRACKER_SONGCORE_AUTOMATION_H

// AUS / AUF: AUS beside a parameter starts a ramp from its value with a curve, a later AUF sets where
// it arrives (`VOL 00 AUS 80` … `AUF FF`). Here: which parameters ramp, and how pairs are found, in
// STEP indices; the scheduler emits them.
// ⚠️ Keep pairing in step space — a pre-scan for frames would double-advance the groove step.

#include <cstdint>
#include <vector>

#include "automation_curve.h"   // the shape, the byte, the one EQ-morph rule — shared with the engine
#include "effects.h"
#include "event.h"
#include "model.h"

namespace songcore {

// ─── The registry: which parameters a ramp can move ───────────────────────────────────────────────
// A ramp is the parameter's own CC, emitted more often. To add one: a CC id, an arm in
// `EngineConsumer::consume`, a queued apply on the audio thread, and a row here.
// BYTE endpoints are values; EQ_PRESET endpoints are preset slots, and the bands between them move.
enum class RampKind { BYTE, EQ_PRESET, VIBRATO };

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
    // PVB / PVX: speed and depth in one byte, each nibble ramped on its own; ticks are EV_EXT_VIBRATO.
    { FX_PVB,    0,              false, RampKind::VIBRATO },
    { FX_PVX,    0,              false, RampKind::VIBRATO },
    // ⚠️ Global: on a track's own lane, the EXTERNAL-routing gate would swallow the master fade on
    // any track playing a MIDI instrument (event.h).
    { FX_VMV,    CC_MASTER_VOL,  true,  RampKind::BYTE },
    // TIM: the delay head glides and shifts pitch — the ramp is the point of the command. Global,
    // with the same restore-on-stop debt as VMV.
    { FX_TIM,    CC_DELAY_TIME,  true,  RampKind::BYTE },
    // ── The EQ presets emit a band set, not a controller value ──
    // ⚠️ EQN is per VOICE: a note-on resets it, so the morph re-asserts a tic after each retrigger.
    { FX_EQN,    0,              false, RampKind::EQ_PRESET },
    // ⚠️ EQM replaces the master EQ and holds; the host restores it on stop().
    { FX_EQM,    0,              true,  RampKind::EQ_PRESET },
};

inline constexpr int AUTOMATABLE_PARAM_COUNT =
    static_cast<int>(sizeof(AUTOMATABLE_PARAMS) / sizeof(AutomatableParam));

// The registry row for an effect code, or nullptr.
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

// What an EQ_PRESET ramp holds at `t` (the band rule is `automation_eq_band_at`). Slots are clamped:
// a hand-edited file can hold an out-of-range one.
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

// ─── The pairing ─────────────────────────────────────────────────────────────────────────────────

static_assert(RAMP_AUS_CODE == FX_AUS && RAMP_AUF_CODE == FX_AUF, "automation_curve.h's AUS/AUF");
static_assert(RAMP_EQ_PRESET_SLOTS == POOL_EQPRESETS, "automation_curve.h's EQ preset slots");

// One ramp as seen from ONE phrase: `span` and `stepOffset` are the whole ramp's; `ausStep`/`aufStep`
// and the slots are this phrase's, −1/0 when that end is elsewhere.
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

/** …and of those, the ones a note-on carries in its own fields rather than as a controller. */
inline bool ramp_rides_note_on(const RampSpec& r) {
    return r.ccId == CC_VOLUME || r.ccId == CC_PAN || r.kind == RampKind::VIBRATO;
}

/** The byte a BYTE or VIBRATO ramp holds at `t`. */
inline int ramp_value(const RampSpec& r, double t) {
    return r.kind == RampKind::VIBRATO ? automation_value_nibbles(r.startByte, r.destByte, r.curveByte, t)
                                       : automation_value_byte(r.startByte, r.destByte, r.curveByte, t);
}

/** Does this phrase carry an AUS or AUF cell at all? A cheap skip — never a substitute for pairing. */
inline bool phrase_has_ramp_cell(const Phrase& phrase) {
    for (const PhraseStep& authored : phrase.steps) {
        const PhraseStep step = as_played(authored);
        for (int slot = 1; slot <= 3; ++slot) {
            const int type = step_fx_type(step, slot);
            if (type == FX_AUS || type == FX_AUF) return true;
        }
    }
    return false;
}

// How a phrase's effect answers the pairing.
inline RampArm ramp_arm(int fxType) {
    const AutomatableParam* p = automatable_param(fxType);
    if (p == nullptr) return RampArm::NONE;
    return p->kind == RampKind::EQ_PRESET ? RampArm::EQ_SLOT : RampArm::VALUE;
}

// A step as the walk reads it: as played, before CHA/RND/RNL. `ausHere` false hides its AUS cells —
// a HOP entered the phrase below them.
inline RampRowFx ramp_step_fx(const PhraseStep& step, bool ausHere) {
    RampRowFx fx = ramp_row_fx(as_played(step));
    if (!ausHere)
        for (int slot = 1; slot <= 3; ++slot)
            if (fx.type[slot] == FX_AUS) fx.type[slot] = FX_NONE;
    return fx;
}

// The parameter and both ends of a pair; the caller places it in its phrase.
inline RampSpec ramp_spec(const RampPair& p) {
    const AutomatableParam* param = automatable_param(p.paramType);   // non-null: `ramp_arm` let it in
    RampSpec r;
    r.fxCode     = param->fxCode;
    r.ccId       = param->ccId;
    r.global     = param->global;
    r.kind       = param->kind;
    r.paramSlot  = p.paramSlot;
    r.ausSlot    = p.ausSlot;
    r.aufSlot    = p.aufSlot;
    r.startByte  = p.startByte;
    r.destByte   = p.destByte;
    r.curveByte  = p.curveByte;
    r.span       = p.aufPos - p.ausPos;
    r.originAbs  = p.ausPos;
    r.originSlot = p.ausSlot;
    return r;
}

// Every ramp `phrase` declares, by `pair_ramps`. An AUS above `startRow` (a HOP entry) is skipped.
inline std::vector<RampSpec> find_ramps(const Phrase& phrase, int startRow = 0) {
    std::vector<RampSpec> out;
    pair_ramps(static_cast<int>(phrase.steps.size()),
               [&](int step) { return ramp_step_fx(phrase.steps[static_cast<size_t>(step)], step >= startRow); },
               ramp_arm,
               [&](const RampPair& p) {
                   RampSpec r   = ramp_spec(p);
                   r.ausStep    = p.ausPos;
                   r.aufStep    = p.aufPos;
                   r.stepOffset = -p.ausPos;   // see RampSpec: elapsed = stepOffset + stepIndex
                   out.push_back(r);
               });
    return out;
}

// ─── Pairing across a chain: the AUF may sit in a later phrase of the SAME chain ─────────────────
// ⚠️ The chain is the boundary, and pairs are re-derived from (chain, chainRow) on every call, never
// carried — a chain can be re-entered and a LIVE rewind schedules a phrase twice.

// Every ramp playing over the phrase at `chainRow`, opened there or earlier: one walk over the rows
// that play, end to end, PHRASE_ROWS absolute steps each. `startRow` (a HOP entry) applies only to
// ramps opening here. Steps are authored ones: a HOP inside the span runs the middle of the fade
// slightly ahead; both ends stay exact.
inline std::vector<RampSpec> find_ramps_in_chain(const Project& project, const Chain& chain,
                                                 int chainRow, int startRow = 0) {
    std::vector<RampSpec> out;
    if (chainRow < 0 || chainRow >= CHAIN_ROWS || chain_phrase_ref(chain, chainRow) < 0) return out;

    const Phrase* played[CHAIN_ROWS];
    int           count = 0;
    int           here  = 0;
    for (int row = 0; row < CHAIN_ROWS; ++row) {
        const int ref = chain_phrase_ref(chain, row);
        if (ref < 0) continue;
        if (row == chainRow) here = count;
        played[count++] = &project.phrases[static_cast<size_t>(ref)];
    }
    const int hereAbs = here * PHRASE_ROWS;
    const int hereEnd = hereAbs + PHRASE_ROWS - 1;
    const auto inHere = [&](int abs) { return abs >= hereAbs && abs <= hereEnd; };

    pair_ramps(count * PHRASE_ROWS,
               [&](int abs) {
                   const Phrase& phrase = *played[abs / PHRASE_ROWS];
                   const int     step   = abs % PHRASE_ROWS;
                   if (step >= static_cast<int>(phrase.steps.size())) return RampRowFx{};
                   return ramp_step_fx(phrase.steps[static_cast<size_t>(step)],
                                       !inHere(abs) || step >= startRow);
               },
               ramp_arm,
               [&](const RampPair& p) {
                   if (p.aufPos < hereAbs || p.ausPos > hereEnd) return;   // not over this phrase
                   RampSpec r   = ramp_spec(p);
                   r.stepOffset = hereAbs - p.ausPos;
                   r.ausStep    = inHere(p.ausPos) ? p.ausPos - hereAbs : -1;
                   r.aufStep    = inHere(p.aufPos) ? p.aufPos - hereAbs : -1;
                   if (r.ausStep < 0) r.ausSlot = 0;   // that cell is in another phrase
                   if (r.aufStep < 0) r.aufSlot = 0;
                   out.push_back(r);
               });
    return out;
}

// ─── Which cells of ONE phrase a ramp uses, for the editor: live if ANY chain context uses it ─────

// `[step][slot]`, slots 1-3. Ends in another phrase are skipped, so marking several rows is a union.
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

// Every AUS/AUF cell of the phrase a ramp uses, over every chain row that plays it — through
// `find_ramps_in_chain`, so the editor cannot show a fade that will not play.
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
