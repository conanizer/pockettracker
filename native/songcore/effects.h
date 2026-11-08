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

// Effect code → 3-letter display name, or "---". (RPT is FX_REPEAT; DEL/REV are the sends.)
inline std::string effect_name(int code) {
    switch (code) {
        case FX_ARC: return "ARC"; case FX_CHA: return "CHA"; case FX_LAT: return "LAT";
        case FX_GRV: return "GRV"; case FX_HOP: return "HOP"; case FX_TIC: return "TIC";
        case FX_ARPEGGIO: return "ARP"; case FX_KILL: return "KIL"; case FX_OFFSET: return "OFF";
        case FX_RND: return "RND"; case FX_RNL: return "RNL"; case FX_REPEAT: return "RPT";
        case FX_TBL: return "TBL"; case FX_THO: return "THO"; case FX_VOLUME: return "VOL";
        case FX_PSL: return "PSL"; case FX_PBN: return "PBN"; case FX_PVB: return "PVB";
        case FX_PVX: return "PVX"; case FX_PIT: return "PIT"; case FX_SLI: return "SLI";
        case FX_PAN: return "PAN"; case FX_RSEND: return "REV"; case FX_DSEND: return "DEL";
        case FX_BCK: return "BCK"; case FX_EQN: return "EQN"; case FX_EQM: return "EQM";
        case FX_CUT: return "CUT"; case FX_RES: return "RES";
        case FX_LPF: return "LPF"; case FX_HPF: return "HPF"; case FX_BPF: return "BPF";
        case FX_DRV: return "DRV"; case FX_CRU: return "CRU";
        case FX_FIN: return "FIN"; case FX_TSX: return "TSX"; case FX_LPO: return "LPO";
        case FX_INS: return "INS"; case FX_TIM: return "TIM"; case FX_NTH: return "NTH";
        case FX_CHD: return "CHD";
        case FX_VTR: return "VTR"; case FX_VMV: return "VMV";
        case FX_SCA: return "SCA"; case FX_SCG: return "SCG";
        case FX_AUS: return "AUS"; case FX_AUF: return "AUF";
        case FX_MPG: return "MPG"; case FX_MPB: return "MPB";
        case FX_CCA: return "CCA"; case FX_CCB: return "CCB";
        case FX_CCC: return "CCC"; case FX_CCD: return "CCD";
        default: return "---";
    }
}

// Max parameter byte: pool refs and MPG 0x7F, SCA/SCG 0xBF, the rest 0xFF.
inline constexpr int effect_value_max(int effect_type) {
    if (effect_type == FX_SCA || effect_type == FX_SCG) return 0xBF;
    return (effect_type == FX_TBL || effect_type == FX_GRV ||
            effect_type == FX_EQN || effect_type == FX_EQM ||
            effect_type == FX_MPG || effect_type == FX_INS) ? 127 : 255;
}

/** The two halves of an SCA/SCG byte. The key is clamped because a hand-written file can hold one the
 *  editor refuses; four bits is exactly the slot pool. */
inline constexpr int scale_cmd_key(int value) { return ((value >> 4) & 0x0F) > 11 ? 11 : ((value >> 4) & 0x0F); }
inline constexpr int scale_cmd_slot(int value) { return value & 0x0F; }

// The FX column's cycle order and the picker's reading order. A `.ptp` stores the CODE, so re-ordering
// changes no saved cell — but it moves the picker, the input tests and the hidden tails below.
inline constexpr int EFFECT_TYPES[] = {
    FX_NONE, FX_ARC, FX_CHA, FX_LAT, FX_GRV, FX_HOP, FX_TIC, FX_ARPEGGIO, FX_KILL, FX_OFFSET,
    FX_RND, FX_RNL, FX_REPEAT, FX_TBL, FX_THO, FX_VOLUME,
    FX_PSL, FX_PBN, FX_PVB, FX_PVX, FX_PIT, FX_SLI,
    FX_PAN, FX_BCK, FX_RSEND, FX_DSEND, FX_EQN, FX_EQM,
    // Mixer faders
    FX_VTR, FX_VMV,
    // The automation pair
    FX_AUS, FX_AUF,
    // ⚠️ From here on, entries are APPENDED, not placed by subject: inserting would renumber the
    // indices the recorded EDIT test cases carry, and those have no independent author left.
    FX_CUT, FX_RES,
    FX_SCA, FX_SCG,
    FX_LPF, FX_HPF, FX_BPF,
    FX_DRV, FX_CRU,
    FX_FIN,
    FX_TSX,
    // INS, TIM, NTH and CHD sit above LPO because the tail is what a release build trims.
    FX_INS,
    FX_TIM,
    FX_NTH,
    FX_CHD,
    FX_LPO,
    // The MIDI commands — must stay the LAST six (asserted below)
    FX_MPG, FX_MPB, FX_CCA, FX_CCB, FX_CCC, FX_CCD,
};
inline constexpr int EFFECT_TYPE_COUNT = static_cast<int>(sizeof(EFFECT_TYPES) / sizeof(int));

// ⚠️ A build without MIDI (platform_caps.h `midi`) shows only the first EFFECT_TYPE_COUNT_NO_MIDI
// entries. That is safe only because the six are a TAIL: every remaining index keeps its meaning.
inline constexpr int MIDI_EFFECT_COUNT         = 6;
inline constexpr int EFFECT_TYPE_COUNT_NO_MIDI = EFFECT_TYPE_COUNT - MIDI_EFFECT_COUNT;

static_assert(EFFECT_TYPES[EFFECT_TYPE_COUNT_NO_MIDI + 0] == FX_MPG &&
                  EFFECT_TYPES[EFFECT_TYPE_COUNT_NO_MIDI + 1] == FX_MPB &&
                  EFFECT_TYPES[EFFECT_TYPE_COUNT_NO_MIDI + 2] == FX_CCA &&
                  EFFECT_TYPES[EFFECT_TYPE_COUNT_NO_MIDI + 3] == FX_CCB &&
                  EFFECT_TYPES[EFFECT_TYPE_COUNT_NO_MIDI + 4] == FX_CCC &&
                  EFFECT_TYPES[EFFECT_TYPE_COUNT_NO_MIDI + 5] == FX_CCD,
              "the MIDI commands must stay the LAST six entries of EFFECT_TYPES — a build that hides "
              "them shortens the list, so anything after them would be hidden too");

// ⚠️ LPO is hidden from release builds the same way and must sit directly below the MIDI six: the
// trims nest.
inline constexpr int PREVIEW_EFFECT_COUNT     = 1;   // FX_LPO
inline constexpr int EFFECT_TYPE_COUNT_STABLE = EFFECT_TYPE_COUNT_NO_MIDI - PREVIEW_EFFECT_COUNT;

static_assert(EFFECT_TYPES[EFFECT_TYPE_COUNT_STABLE] == FX_LPO,
              "LPO must sit directly below the MIDI six — a build that hides it shortens the list, "
              "so an effect moved after it would be hidden too");

/** Index of `code` in EFFECT_TYPES, or 0 (FX_NONE) for an unknown effect. */
inline int effect_type_index(int code) {
    for (int i = 0; i < EFFECT_TYPE_COUNT; ++i)
        if (EFFECT_TYPES[i] == code) return i;
    return 0;
}

/** EFFECT_TYPES[i], or FX_NONE when out of range. */
inline int effect_type_at(int index) {
    return (index >= 0 && index < EFFECT_TYPE_COUNT) ? EFFECT_TYPES[index] : FX_NONE;
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
    std::optional<int> reverbSendValue;
    std::optional<int> delaySendValue;
    std::optional<int> bckValue;
    std::optional<int> filterCutValue;    // CUT
    std::optional<int> filterResValue;    // RES
    // LPF / HPF / BPF: one optional for all three, so two on one step resolve last-wins like any
    // other pair instead of mixing one's type with the other's cutoff.
    std::optional<int> filterModeValue;   // LPF/HPF/BPF cutoff byte
    int filterModeType = 0;               // 1 lp | 2 hp | 3 bp; meaningless unless the above is set
    std::optional<int> driveValue;        // DRV
    std::optional<int> crushValue;        // CRU — both nibbles still packed, split at the engine
    std::optional<int> fineTuneValue;     // FIN — authored byte, turned into semitones at the engine
    std::optional<int> tsxMultiplier;     // TSX — already decoded to a signed multiplier
    std::optional<int> loopSlideValue;    // LPO — already decoded to signed sixteenths of a loop
    std::optional<int> chordValue;        // CHD — the authored byte (chords.h)
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
            case FX_RSEND:  p.reverbSendValue = value; break;
            case FX_DSEND:  p.delaySendValue = value; break;
            case FX_BCK:    p.bckValue = value; break;
            case FX_CUT:    p.filterCutValue = value; break;
            case FX_RES:    p.filterResValue = value; break;
            case FX_LPF: case FX_HPF: case FX_BPF:
                p.filterModeValue = value;
                p.filterModeType  = fx_filter_mode(type);
                break;
            case FX_DRV:    p.driveValue     = value; break;
            case FX_CRU:    p.crushValue     = value; break;
            case FX_FIN:    p.fineTuneValue  = value; break;
            // Sign-decoded here, like PIT, so the scheduler gets a number.
            case FX_TSX:    p.tsxMultiplier = (value < 0x80) ? value : value - 256; break;
            case FX_LPO:    p.loopSlideValue = loop_slide_sixteenths(value); break;
            case FX_CHD:    p.chordValue = value & 0xFF; arcStrums = true; break;
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
            // ARP / CHA / RND / RNL / TIC and unknown codes: nothing to resolve.
            default: break;
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
