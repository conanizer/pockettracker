#ifndef POCKETTRACKER_SONGCORE_EFFECTS_H
#define POCKETTRACKER_SONGCORE_EFFECTS_H

// ─── Effect resolution ────────────────────────────────────────────────────────────────────────────
//
// 1:1 port of core/logic/EffectProcessor.kt: the FX_* effect codes, the ResolvedStepParams bundle,
// and resolveStepParams() — the pure function that folds a PhraseStep's three FX slots into the
// parameter bundle the scheduler consumes. Stateless; last-wins across the three slots (resolved
// 1 → 3), matching the Kotlin `when` overwrite order.
//
// The Kotlin TRACE logging is intentionally dropped — it is debug-only and never touches the resolved
// output. Effects with no resolved field (ARP / CHA / RND / RNL / TIC) are no-ops here, exactly as
// their `when` arms are: CHA/RND/RNL gate or rewrite the step BEFORE resolution (in the scheduler),
// ARP/TIC are applied from track state / the table engine.
//
// EffectProcessor.kt is the executable spec. tools/ptresolve proves this against a JVM golden.

#include <cstdint>
#include <optional>
#include <string>
#include "model.h"

namespace songcore {

// ─── Effect codes (EffectProcessor companion — single source of truth) ────────────────────────────
constexpr int FX_NONE     = 0x00;
constexpr int FX_ARC      = 0x03;  // Cxx  arpeggio config (mode/speed)
constexpr int FX_CHA      = 0x04;  // CHA  chance gate
constexpr int FX_LAT      = 0x05;  // LAT  latency (delay row trigger by xx ticks)
constexpr int FX_GRV      = 0x07;  // GRV  assign groove table
constexpr int FX_HOP      = 0x08;  // Hxx  hop (FF = stop track)
constexpr int FX_TIC      = 0x09;  // Txx  table tick rate (resolved elsewhere)
constexpr int FX_ARPEGGIO = 0x0A;  // Axx  arpeggio (applied from track state)
constexpr int FX_KILL     = 0x0B;  // K00  kill sample
constexpr int FX_OFFSET   = 0x0F;  // Oxx  sample start point
constexpr int FX_RND      = 0x10;  // RND  randomize previous FX column
constexpr int FX_RNL      = 0x11;  // RNL  randomize FX column to the left
constexpr int FX_REPEAT   = 0x12;  // Rxy  retrigger
constexpr int FX_TBL      = 0x14;  // TBL  set table for this instrument
constexpr int FX_THO      = 0x15;  // THO  table hop
constexpr int FX_VOLUME   = 0x16;  // Vxx  volume automation
constexpr int FX_PSL      = 0x19;  // PSL  pitch slide (portamento)
constexpr int FX_PBN      = 0x1A;  // PBN  pitch bend
constexpr int FX_PVB      = 0x1B;  // PVB  vibrato
constexpr int FX_PVX      = 0x1C;  // PVX  extreme vibrato
constexpr int FX_PIT      = 0x1D;  // PIT  pitch offset (signed semitones)
constexpr int FX_SLI      = 0x1E;  // SLI  slice index override
constexpr int FX_PAN      = 0x1F;  // PAN  per-note pan
constexpr int FX_RSEND    = 0x20;  // REV  per-note reverb send
constexpr int FX_DSEND    = 0x21;  // DEL  per-note delay send
constexpr int FX_BCK      = 0x22;  // BCK  playback direction
constexpr int FX_EQN      = 0x23;  // EQN  per-note EQ preset slot
constexpr int FX_EQM      = 0x24;  // EQM  master/mixer EQ preset slot

// Effect code → 3-letter display name, or "---" for NONE/unknown. Mirrors FX_NAMES / effectName().
// (RPT is the on-screen name for FX_REPEAT; DEL/REV are the DSEND/RSEND labels.)
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
        default: return "---";
    }
}

// Max parameter byte for an effect: table/groove/EQ-preset pool refs cap at 0x7F (128 slots), all
// others use the full 0xFF range. Mirrors effectValueMax().
inline int effect_value_max(int effect_type) {
    return (effect_type == FX_TBL || effect_type == FX_GRV ||
            effect_type == FX_EQN || effect_type == FX_EQM) ? 127 : 255;
}

// The cycle order of the FX-type column, and the reading order of the FX helper grid — mirrors
// EffectProcessor.EFFECT_TYPES exactly, including the trailing send/EQ group that the helper draws
// as a centred last row. This is a UI-facing list (an FX column stores an INDEX into it, and A+UP
// steps that index), but it lives here beside the codes and the names because those three must never
// drift: an entry added to the effects without an entry here is an effect no one can type.
inline constexpr int EFFECT_TYPES[] = {
    FX_NONE, FX_ARC, FX_CHA, FX_LAT, FX_GRV, FX_HOP, FX_TIC, FX_ARPEGGIO, FX_KILL, FX_OFFSET,
    FX_RND, FX_RNL, FX_REPEAT, FX_TBL, FX_THO, FX_VOLUME,
    FX_PSL, FX_PBN, FX_PVB, FX_PVX, FX_PIT, FX_SLI,
    // Last grid row (centred): the four send/EQ FX
    FX_PAN, FX_BCK, FX_RSEND, FX_DSEND, FX_EQN, FX_EQM,
};
inline constexpr int EFFECT_TYPE_COUNT = static_cast<int>(sizeof(EFFECT_TYPES) / sizeof(int));

/** Index of `code` in EFFECT_TYPES, or 0 (FX_NONE) if it is not a known effect — `indexOf(...) ?: 0`. */
inline int effect_type_index(int code) {
    for (int i = 0; i < EFFECT_TYPE_COUNT; ++i)
        if (EFFECT_TYPES[i] == code) return i;
    return 0;
}

/** EFFECT_TYPES[i], or FX_NONE when out of range — Kotlin's `getOrElse(i) { FX_NONE }`. */
inline int effect_type_at(int index) {
    return (index >= 0 && index < EFFECT_TYPE_COUNT) ? EFFECT_TYPES[index] : FX_NONE;
}

// ─── Resolved bundle (ResolvedStepParams) ─────────────────────────────────────────────────────────
// std::optional mirrors Kotlin's nullable `Int?` / `Long?` ("effect not present on this step"); the
// non-optional fields carry the same defaults as the Kotlin data class.
struct ResolvedStepParams {
    int   startPoint    = -1;      // -1 = use instrument default
    float volume        = 1.0f;
    bool  volumeFromVxx = false;   // true when set by Vxx, not the step volume column
    std::optional<int64_t> killAtFrame;
    int   killOffsetTicks = 0;     // KIL xx: extra ticks between the KIL row and the actual stop
    std::optional<int> arcValue;
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
    std::optional<int> eqnSlot;
    std::optional<int> eqmSlot;
};

// Fold a step's three FX slots into the resolved bundle. `default_volume` seeds `volume` (the
// instrument volume the caller passes); a Vxx effect overrides it. `base_frame` is echoed into
// killAtFrame by a KIL effect. Mirrors EffectProcessor.resolveStepParams().
inline ResolvedStepParams resolve_step_params(const PhraseStep& step,
                                              int64_t base_frame, float default_volume) {
    ResolvedStepParams p;
    p.volume = default_volume;

    for (int fxSlot = 1; fxSlot <= 3; ++fxSlot) {
        int type, value;
        switch (fxSlot) {
            case 1:  type = step.fx1Type; value = step.fx1Value; break;
            case 2:  type = step.fx2Type; value = step.fx2Value; break;
            default: type = step.fx3Type; value = step.fx3Value; break;
        }

        switch (type) {
            case FX_OFFSET: p.startPoint = value; break;
            case FX_VOLUME: p.volume = value / 255.0f; p.volumeFromVxx = true; break;
            case FX_KILL:   p.killAtFrame = base_frame; p.killOffsetTicks = value; break;
            case FX_ARC:    p.arcValue = value; break;
            case FX_REPEAT: {
                // M8-style RXY: y!=0 → retrig every y ticks + vol ramp x; y=0 → retrig every x ticks.
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
            case FX_EQN:    p.eqnSlot = value; break;
            case FX_EQM:    p.eqmSlot = value; break;
            case FX_TBL:    p.tableOverride = value; break;
            case FX_THO:    p.tableHopTarget = value; break;
            case FX_GRV:    p.grooveId = value; break;
            case FX_PIT:    p.pitSemitones = (value < 0x80) ? value : value - 256; break;
            case FX_SLI:    p.sliIndex = value; break;
            // ARP / CHA / RND / RNL / TIC and any unknown code: no resolved field — no-op.
            default: break;
        }
    }
    return p;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_EFFECTS_H
