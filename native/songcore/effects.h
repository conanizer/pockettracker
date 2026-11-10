#ifndef POCKETTRACKER_SONGCORE_EFFECTS_H
#define POCKETTRACKER_SONGCORE_EFFECTS_H

// What a step's commands mean: `resolve_step_params` folds its three FX slots into one bundle,
// last-wins from slot 1 to 3. CHA/RND/RNL rewrite the step before it (scheduler); ARP and TIC are
// track state. The codes themselves are in effect_codes.h.

#include <cstdint>
#include <optional>
#include <string>
#include "effect_codes.h"
#include "model.h"
#include "voice_commands.h"

namespace songcore {

/** Every NTH on the row must agree. */
inline bool step_plays_on_pass(const PhraseStep& s, int pass) {
    for (const FxSlot& f : s.fx)
        if (f.type == FX_NTH && !nth_plays(f.value, pass)) return false;
    return true;
}

// The instrument an INS cell on this step names, or -1. Rightmost wins.
inline int step_ins_instrument(const PhraseStep& s) {
    for (int slot = FX_SLOTS; slot >= 1; --slot)
        if (fx_slot(s, slot).type == FX_INS) return fx_slot(s, slot).value & 0xFF;
    return -1;
}

/** Slot index 0-3 for FX_CCA..FX_CCD, or -1 for any other effect code. */
inline int fx_cc_slot(int code) {
    return (code >= FX_CCA && code <= FX_CCD) ? code - FX_CCA : -1;
}

// ─── Every effect, in the FX column's cycle order and the picker's reading order ─────────────────
// A cell stores the CODE, so re-ordering changes no saved song — but it moves the picker and the
// indices the input tests type.
// ⚠️ A new effect goes directly above LPO: a release build hides LPO and a build without MIDI the six
// MIDI commands, both by cutting the tail, so the trims nest (asserted below).
struct EffectDef {
    int         code;
    const char* name;   // RPT is FX_REPEAT; REV and DEL are the sends
    int         max;    // the largest value byte the cell takes
};

inline constexpr EffectDef EFFECTS[] = {
    {FX_NONE, "---", 0xFF},
    {FX_ARC, "ARC", 0xFF}, {FX_CHA, "CHA", 0xFF}, {FX_LAT, "LAT", 0xFF}, {FX_GRV, "GRV", 0x7F},
    {FX_HOP, "HOP", 0xFF}, {FX_TIC, "TIC", 0xFF}, {FX_ARPEGGIO, "ARP", 0xFF}, {FX_KILL, "KIL", 0xFF},
    {FX_OFFSET, "OFF", 0xFF}, {FX_RND, "RND", 0xFF}, {FX_RNL, "RNL", 0xFF}, {FX_REPEAT, "RPT", 0xFF},
    {FX_TBL, "TBL", 0x7F}, {FX_THO, "THO", 0xFF}, {FX_VOLUME, "VOL", 0xFF},
    {FX_PSL, "PSL", 0xFF}, {FX_PBN, "PBN", 0xFF}, {FX_PVB, "PVB", 0xFF}, {FX_PVX, "PVX", 0xFF},
    {FX_PIT, "PIT", 0xFF}, {FX_SLI, "SLI", 0xFF},
    {FX_PAN, "PAN", 0xFF}, {FX_BCK, "BCK", 0xFF}, {FX_RSEND, "REV", 0xFF}, {FX_DSEND, "DEL", 0xFF},
    {FX_EQN, "EQN", 0x7F}, {FX_EQM, "EQM", 0x7F},
    {FX_VTR, "VTR", 0xFF}, {FX_VMV, "VMV", 0xFF},
    {FX_AUS, "AUS", 0xFF}, {FX_AUF, "AUF", 0xFF},
    {FX_CUT, "CUT", 0xFF}, {FX_RES, "RES", 0xFF},
    {FX_SCA, "SCA", 0xBF}, {FX_SCG, "SCG", 0xBF},
    {FX_LPF, "LPF", 0xFF}, {FX_HPF, "HPF", 0xFF}, {FX_BPF, "BPF", 0xFF},
    {FX_DRV, "DRV", 0xFF}, {FX_CRU, "CRU", 0xFF},
    {FX_FIN, "FIN", 0xFF},
    {FX_TSX, "TSX", 0xFF},
    {FX_INS, "INS", 0x7F},
    {FX_TIM, "TIM", 0xFF},
    {FX_NTH, "NTH", 0xFF},
    {FX_CHD, "CHD", 0xFF},
    {FX_LEG, "LEG", 0xFF},
    {FX_LPO, "LPO", 0xFF},
    {FX_MPG, "MPG", 0x7F}, {FX_MPB, "MPB", 0xFF},
    {FX_CCA, "CCA", 0xFF}, {FX_CCB, "CCB", 0xFF}, {FX_CCC, "CCC", 0xFF}, {FX_CCD, "CCD", 0xFF},
};
inline constexpr int EFFECT_TYPE_COUNT = static_cast<int>(sizeof(EFFECTS) / sizeof(EffectDef));

/** The row for `code`, or nullptr for an unknown one. */
inline constexpr const EffectDef* effect_def(int code) {
    for (const EffectDef& e : EFFECTS)
        if (e.code == code) return &e;
    return nullptr;
}

// Effect code → its 3-letter name, or "---".
inline std::string effect_name(int code) {
    const EffectDef* e = effect_def(code);
    return e != nullptr ? e->name : "---";
}

// The largest value byte; 0xFF for an unknown code.
inline constexpr int effect_value_max(int code) {
    const EffectDef* e = effect_def(code);
    return e != nullptr ? e->max : 0xFF;
}

/** The two halves of an SCA/SCG byte. The key is clamped because a hand-written file can hold one the
 *  editor refuses; four bits is exactly the slot pool. */
inline constexpr int scale_cmd_key(int value) { return ((value >> 4) & 0x0F) > 11 ? 11 : ((value >> 4) & 0x0F); }
inline constexpr int scale_cmd_slot(int value) { return value & 0x0F; }

// ⚠️ A build without MIDI (platform_caps.h `midi`) shows only the first EFFECT_TYPE_COUNT_NO_MIDI
// entries. That is safe only because the six are a TAIL: every remaining index keeps its meaning.
inline constexpr int MIDI_EFFECT_COUNT         = 6;
inline constexpr int EFFECT_TYPE_COUNT_NO_MIDI = EFFECT_TYPE_COUNT - MIDI_EFFECT_COUNT;

static_assert(EFFECTS[EFFECT_TYPE_COUNT_NO_MIDI + 0].code == FX_MPG &&
                  EFFECTS[EFFECT_TYPE_COUNT_NO_MIDI + 1].code == FX_MPB &&
                  EFFECTS[EFFECT_TYPE_COUNT_NO_MIDI + 2].code == FX_CCA &&
                  EFFECTS[EFFECT_TYPE_COUNT_NO_MIDI + 3].code == FX_CCB &&
                  EFFECTS[EFFECT_TYPE_COUNT_NO_MIDI + 4].code == FX_CCC &&
                  EFFECTS[EFFECT_TYPE_COUNT_NO_MIDI + 5].code == FX_CCD,
              "the MIDI commands must stay the LAST six entries of EFFECTS — a build that hides "
              "them shortens the list, so anything after them would be hidden too");

// ⚠️ LPO is hidden from release builds the same way and must sit directly below the MIDI six.
inline constexpr int PREVIEW_EFFECT_COUNT     = 1;   // FX_LPO
inline constexpr int EFFECT_TYPE_COUNT_STABLE = EFFECT_TYPE_COUNT_NO_MIDI - PREVIEW_EFFECT_COUNT;

static_assert(EFFECTS[EFFECT_TYPE_COUNT_STABLE].code == FX_LPO,
              "LPO must sit directly below the MIDI six — a build that hides it shortens the list, "
              "so an effect moved after it would be hidden too");

/** Index of `code` in EFFECTS, or 0 (FX_NONE) for an unknown effect. */
inline int effect_type_index(int code) {
    for (int i = 0; i < EFFECT_TYPE_COUNT; ++i)
        if (EFFECTS[i].code == code) return i;
    return 0;
}

/** EFFECTS[i]'s code, or FX_NONE when out of range. */
inline int effect_type_at(int index) {
    return (index >= 0 && index < EFFECT_TYPE_COUNT) ? EFFECTS[index].code : FX_NONE;
}

// The first row with a HOP that fires on every pass (no NTH, no CHA that could gate it), or -1.
// A hint for the editor only: the phrase before can still HOP in below this row.
inline int phrase_certain_hop_row(const Phrase& phrase) {
    const auto unused = [](int type) { return type == FX_NONE || type == FX_NTH; };
    for (int row = 0; row < static_cast<int>(phrase.steps.size()); ++row) {
        const PhraseStep step = as_played(phrase.steps[static_cast<size_t>(row)]);
        bool everyPass = true;
        for (const FxSlot& f : step.fx)
            if (f.type == FX_NTH && !nth_always(f.value)) everyPass = false;
        if (!everyPass) continue;
        for (int slot = 1; slot <= FX_SLOTS; ++slot) {
            if (fx_slot(step, slot).type != FX_HOP) continue;
            int left = slot - 1, right = slot + 1;
            while (left >= 1 && unused(fx_slot(step, left).type)) --left;
            while (right <= FX_SLOTS && unused(fx_slot(step, right).type)) ++right;
            // The CHA to the right gates leftwards with its high nibble, the one to the left with its low.
            const FxSlot& r = fx_slot(step, right);
            const FxSlot& l = fx_slot(step, left);
            const bool gatedFromRight = r.type == FX_CHA && ((r.value >> 4) & 0x0F) != 0x0F;
            const bool gatedFromLeft  = l.type == FX_CHA && (l.value & 0x0F) != 0x0F;
            if (!gatedFromRight && !gatedFromLeft) return row;
        }
    }
    return -1;
}

// ─── Resolved bundle ──────────────────────────────────────────────────────────────────────────────
// An empty optional means "effect not present on this step".
struct ResolvedStepParams {
    int   startPoint    = -1;      // -1 = use instrument default
    float volume        = 1.0f;
    bool  volumeFromVxx = false;   // true when set by Vxx, not the step volume column
    std::optional<int64_t> killAtFrame;
    int   killOffsetTicks = 0;     // KIL xx: extra ticks between the KIL row and the actual stop
    std::optional<int> arcValue;          // ARC that sets the arpeggio
    std::optional<int> strumValue;        // ARC that strums the chord: the nearest CHD / ARP on its left is a CHD
    std::optional<int> repeatCount;
    std::optional<int> repeatVolRamp;
    std::optional<int> hopValue;
    std::optional<int> pslDuration;
    std::optional<int> pbnValue;
    std::optional<int> pvbValue;
    std::optional<int> pvxValue;
    std::optional<int> delayTicks;
    std::optional<int> tableOverride;
    std::optional<int> tableHopTarget;
    std::optional<int> grooveId;
    std::optional<int> pitSemitones;
    std::optional<int> sliIndex;
    std::optional<int> panValue;
    std::optional<int> bckValue;
    // REV, DEL, the filter, DRV, CRU, FIN, LPO (voice_commands.h): per lane, the controller the step
    // sends there (0 = none) and the authored byte — the engine decodes it.
    struct VoiceCc { uint8_t ccId = 0; int value = 0; };
    VoiceCc voiceCc[VOICE_LANES];
    std::optional<int> tsxMultiplier;     // TSX — already decoded to a signed multiplier
    std::optional<int> chordValue;        // CHD — the authored byte (chords.h)
    std::optional<int> legatoTicks;       // LEG — how long the slide takes, 0 = at once
    std::optional<int> eqnSlot;
    std::optional<int> eqmSlot;
    // SCA / SCG: the authored byte, split where it is applied so the key clamp lives in one place.
    std::optional<int> scaleTrackByte;   // SCA
    std::optional<int> scaleGlobalByte;  // SCG
    std::optional<int> trackVolValue;   // VTR (authored byte)
    std::optional<int> masterVolValue;  // VMV (authored byte)
    std::optional<int> delayTimeValue;  // TIM (authored byte, always the free 0-2 s scale)
    // `ccSlotValue[i]` is slot A..D's byte; the controller number is resolved consumer-side.
    std::optional<int> midiProgram;                    // MPG
    std::optional<int> midiBend;                       // MPB (authored byte, not the 14-bit value)
    std::optional<int> ccSlotValue[MIDI_CC_SLOTS];     // CCA-CCD
};

// On an instrument played over MIDI OUT, CHD and LEG on one row cannot both act — one bend moves every
// key on the channel — so the leftmost acts. The slot (1-3) of the one that does nothing, or 0.
inline int chord_legato_inert_slot(const PhraseStep& step) {
    int chord = 0, legato = 0;
    for (int slot = 3; slot >= 1; --slot) {
        if (fx_slot(step, slot).type == FX_CHD) chord = slot;
        if (fx_slot(step, slot).type == FX_LEG) legato = slot;
    }
    if (chord == 0 || legato == 0) return 0;
    return chord < legato ? legato : chord;
}

// Fold a step's three FX slots into the bundle. `default_volume` (the instrument volume) seeds
// `volume`; Vxx overrides it. A KIL echoes `base_frame` into killAtFrame.
inline ResolvedStepParams resolve_step_params(const PhraseStep& step,
                                              int64_t base_frame, float default_volume) {
    ResolvedStepParams p;
    p.volume = default_volume;
    bool arcStrums = false;

    for (const FxSlot& f : step.fx) {
        const int type = f.type, value = f.value;

        switch (type) {
            case FX_OFFSET: p.startPoint = value; break;
            case FX_VOLUME: p.volume = value / 255.0f; p.volumeFromVxx = true; break;
            case FX_KILL:   p.killAtFrame = base_frame; p.killOffsetTicks = value; break;
            case FX_ARC:    (arcStrums ? p.strumValue : p.arcValue) = value; break;
            case FX_ARPEGGIO: arcStrums = false; break;
            case FX_REPEAT: {
                // RXY: y!=0 → retrig every y ticks + vol ramp x; y=0 → retrig every x ticks.
                int highNibble = (value >> 4) & 0x0F;
                int lowNibble  = value & 0x0F;
                if (lowNibble != 0) { p.repeatCount = lowNibble;  p.repeatVolRamp = highNibble; }
                else                { p.repeatCount = highNibble; p.repeatVolRamp = 0; }
                break;
            }
            case FX_HOP:    p.hopValue = value; break;
            case FX_PSL:    p.pslDuration = value; break;
            case FX_PBN:    p.pbnValue = value; break;
            case FX_PVB:    p.pvbValue = value; break;
            case FX_PVX:    p.pvxValue = value; break;
            case FX_LAT:    p.delayTicks = value; break;
            case FX_PAN:    p.panValue = value; break;
            case FX_BCK:    p.bckValue = value; break;
            // Sign-decoded here, like PIT, so the scheduler gets a number.
            case FX_TSX:    p.tsxMultiplier = (value < 0x80) ? value : value - 256; break;
            case FX_CHD:    p.chordValue = value & 0xFF; arcStrums = true; break;
            case FX_LEG:    p.legatoTicks = value & 0xFF; break;
            case FX_EQN:    p.eqnSlot = value; break;
            case FX_EQM:    p.eqmSlot = value; break;
            case FX_SCA:    p.scaleTrackByte = value; break;
            case FX_SCG:    p.scaleGlobalByte = value; break;
            case FX_VTR:    p.trackVolValue = value; break;
            case FX_VMV:    p.masterVolValue = value; break;
            case FX_TIM:    p.delayTimeValue = value; break;
            case FX_TBL:    p.tableOverride = value; break;
            case FX_THO:    p.tableHopTarget = value; break;
            case FX_GRV:    p.grooveId = value; break;
            case FX_PIT:    p.pitSemitones = (value < 0x80) ? value : value - 256; break;
            case FX_SLI:    p.sliIndex = value; break;
            case FX_MPG:    p.midiProgram = value & 0x7F; break;
            case FX_MPB:    p.midiBend = value; break;
            case FX_CCA: case FX_CCB: case FX_CCC: case FX_CCD:
                p.ccSlotValue[fx_cc_slot(type)] = value;
                break;
            default:
                if (const VoiceCommand* c = voice_command_for_fx(type))
                    p.voiceCc[c->lane] = {c->ccId, value};
                // ARP / CHA / RND / RNL / TIC and unknown codes: nothing to resolve.
                break;
        }
    }
    return p;
}

// The semitones the chain and project transposes move THIS step, after TRANSP. (off wins) and TSX.
// Sliced instruments are not a case: slice selection already ignores the transpose (voice_derive.h).
inline int effective_transpose_semitones(int transposeSemitones, const Project& project,
                                         int instrumentId,
                                         const std::optional<int>& tsxMultiplier) {
    if (instrumentId >= 0 && instrumentId < static_cast<int>(project.instruments.size()) &&
        !project.instruments[static_cast<size_t>(instrumentId)].transposeEnabled) {
        return 0;
    }
    return transposeSemitones * tsxMultiplier.value_or(1);
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_EFFECTS_H
