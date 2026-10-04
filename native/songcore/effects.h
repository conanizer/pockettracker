#ifndef POCKETTRACKER_SONGCORE_EFFECTS_H
#define POCKETTRACKER_SONGCORE_EFFECTS_H

// ─── Effect resolution ────────────────────────────────────────────────────────────────────────────
//
// The FX_* codes and `resolve_step_params`, which folds a step's three FX slots into one bundle,
// last-wins from slot 1 to 3. Stateless. ARP / CHA / RND / RNL / TIC resolve to nothing here: CHA,
// RND and RNL rewrite the step before resolution (scheduler), ARP and TIC come from track state.

#include <cstdint>
#include <optional>
#include <string>
#include "model.h"

namespace songcore {

// ─── Effect codes ─────────────────────────────────────────────────────────────────────────────────
// ⚠️ A code is a saved cell's identity: never renumber or reuse one.
constexpr int FX_NONE     = 0x00;
constexpr int FX_ARC      = 0x03;  // Cxx  arpeggio config (mode/speed)
constexpr int FX_CHA      = 0x04;  // CHA  chance: x = nearest filled neighbour left (or the note), y = right
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

// ─── The MIDI commands ────────────────────────────────────────────────────────────────────────────
//
// Router events like any other: `CCA` on a SAMPLER moves the mapped engine parameter, on an EXTERNAL
// instrument it sends a controller.
// A CC slot is named by letter; the controller number lives in the instrument, so a re-patched
// instrument is followed. The consumer resolves it (event.h `CC_SLOT_A`) because an FX-only step's
// instrument column is 0x00, not the sounding instrument.
constexpr int FX_MPG      = 0x25;  // MPG  MIDI program change (00-7F)
constexpr int FX_MPB      = 0x26;  // MPB  MIDI pitch bend, absolute (00-FF, 80 = centre)
constexpr int FX_CCA      = 0x27;  // CCA  instrument CC slot A
constexpr int FX_CCB      = 0x28;  // CCB  instrument CC slot B
constexpr int FX_CCC      = 0x29;  // CCC  instrument CC slot C
constexpr int FX_CCD      = 0x2A;  // CCD  instrument CC slot D

// ─── The mixer faders, as effects ─────────────────────────────────────────────────────────────────
//
// ⚠️ VTR REPLACES the track's fader rather than scaling it; the MIXER screen keeps showing the
// authored value. Nothing later puts the engine back, so `SongcoreHost::stop()` restores it
// (`Sequencer::mixer_vol_active()` is the flag) — otherwise the next PLAY starts at the faded level.
constexpr int FX_VTR      = 0x2B;  // VTR  this track's mixer fader (00-FF)
constexpr int FX_VMV      = 0x2C;  // VMV  the master fader (00-FF), global

// ─── The automation pair ──────────────────────────────────────────────────────────────────────────
//
// AUS opens a ramp on the effect to its LEFT and carries the curve; AUF, later, carries the
// destination (automation.h). Neither is resolved below: resolution discards slot position, and AUS
// means "the slot to my left". Pairing reads the slots directly.
constexpr int FX_AUS      = 0x2D;  // AUS  automation start: xx = curve (00 ease-in, 80 linear, FF ease-out)
constexpr int FX_AUF      = 0x2E;  // AUF  automation finish: xx = destination value

// ─── The instrument filter, per note ──────────────────────────────────────────────────────────────
//
// They move the filter the instrument declares and never turn one on: with FILTER TYPE = OFF they
// are inert. Same 00-FF byte as the INSTRUMENT screen's FREQ/RES. Gone at the next note-on, which
// reloads the chain from the instrument.
constexpr int FX_CUT      = 0x2F;  // CUT  filter cutoff  (00-FF)
constexpr int FX_RES      = 0x30;  // RES  filter resonance (00-FF)

// ─── The scale commands ───────────────────────────────────────────────────────────────────────────
//
// High nibble = key (0 = C … 11 = B), low nibble = slot in the 16-scale pool; capped at 0xBF.
// The scale a track is in lives on `TrackState`, like the groove: rewound with LIVE, gone at STOP,
// never saved. SCG is SCA on all eight tracks — the way back for a track SCA moved.
constexpr int FX_SCA      = 0x31;  // SCA  this track's scale: x = key, y = slot
constexpr int FX_SCG      = 0x32;  // SCG  every track's scale, same byte layout

// ─── The filter, switched ON from a cell ──────────────────────────────────────────────────────────
//
// Each sets the type AND the cutoff, so a fresh instrument (FILTER TYPE OFF) can get a filter from the
// FX column. CUT still moves the cutoff without touching the type.
// ⚠️ Type and cutoff ride ONE bus record (CC id = type, value = cutoff): split, the filter would open
// a block before it changes shape — a click at the head of every sweep.
// Notch and peak are deliberately not here; they belong in the INSTRUMENT screen's type list.
constexpr int FX_LPF      = 0x33;  // LPF  low-pass ON at this cutoff  (00-FF)
constexpr int FX_HPF      = 0x34;  // HPF  high-pass ON at this cutoff (00-FF)
constexpr int FX_BPF      = 0x35;  // BPF  band-pass ON at this cutoff (00-FF)

/** The FilterModule type an LPF/HPF/BPF cell turns on (1 lp | 2 hp | 3 bp), or 0 for anything else. */
inline constexpr int fx_filter_mode(int code) {
    return code == FX_LPF ? 1 : code == FX_HPF ? 2 : code == FX_BPF ? 3 : 0;
}

// ─── The two dirt commands ────────────────────────────────────────────────────────────────────────
//
// Read by the sampler's per-block recompute, so they act on the sounding note; reset at the next
// note-on, like CUT/RES.
// ⚠️ 0x36 is retired (a sample-end command that never shipped) and must never be reused.
constexpr int FX_DRV      = 0x37;  // DRV  overdrive amount (00 = clean, FF = heavy)

// One cell, two 4-bit values: left digit crushes bits, right one downsamples.
// ⚠️ Not rampable (automation.h): interpolating the byte wraps the right digit sixteen times.
constexpr int FX_CRU      = 0x38;  // CRU  xy: x = bits crushed (0-F), y = downsample (0-F)

/** The two halves of a CRU byte: bits to crush, and the downsample factor. Both 0 = clean. */
inline constexpr int crush_cmd_bits(int value)       { return (value >> 4) & 0x0F; }
inline constexpr int crush_cmd_downsample(int value) { return value & 0x0F; }

// ─── The cents between the semitones ─────────────────────────────────────────────────────────────
//
// One semitone either way in 256 steps, filling the gap PIT leaves. Retunes the SOUNDING note, so a
// ramp is a pitch bend. On a note step it lands one frame after the trigger (`voiceFxFrame`), like
// CUT/RES and the sends. Reset at the next note.
constexpr int FX_FIN      = 0x39;  // FIN  fine tune (00 = a semitone flat, 80 = in tune, FF = sharp)

// ─── How far a step follows the transposes ───────────────────────────────────────────────────────
//
// A signed multiplier: 01 normal, 02 double, 00 immune, FF mirrored, FE mirrored double. Whole
// semitones only, so no rounding rule.
// It scales the chain TSP column and the project transpose together — the one quantity the instrument's
// TRANSP. switch silences. Phrase only: by the time a table row runs, the transpose is folded into the
// pitch.
constexpr int FX_TSX      = 0x3A;  // TSX  transpose multiplier (00 immune, 01 normal, FF mirrored)

// ─── The loop window, slid ───────────────────────────────────────────────────────────────────────
//
// Moves both ends of the loop together, so the loop length — and the pitch of a looped note — stays.
// The unit is signed sixteenths of the loop's own length: `10` is one loop, `F0` one loop back.
// ⚠️ Cumulative, and the engine keeps the running total in SIXTEENTHS and derives samples from it each
// block: summing rounded per-step offsets would drift off a wave boundary on a loop not divisible
// by 16.
// The window stops at the sample's ends (no wrap) and the count resets on every new note.
constexpr int FX_LPO      = 0x3B;  // LPO  slide the loop window, signed sixteenths of its own length

/** A LPO byte to signed sixteenths of a loop length: 00-7F forward, 80-FF back. */
inline constexpr int loop_slide_sixteenths(int value) { return (value & 0xFF) < 0x80 ? (value & 0xFF)
                                                                                     : (value & 0xFF) - 256; }

// INS xx: this step's note plays instrument `xx` instead of its I column. Applied after CHA/RND/RNL,
// so they can pick the instrument. Phrase only, and only on a step with a note.
constexpr int FX_INS      = 0x3C;  // INS  play this note on instrument xx

// ─── The delay's echo time, from a cell ──────────────────────────────────────────────────────────
//
// Always the free 0-2 s scale, even when the DELAY screen is in sync mode: a ramp needs a continuous
// range, and a cell must not change meaning with a screen setting.
// ⚠️ Replaces the screen's TIME until STOP, like VTR/VMV and EQM: `MixerHeld` (engine_setup.h) keeps a
// mid-take globals push from wiping it, and `SongcoreHost::stop()` restores the project's time.
// The head glides to the new time (delay-module.h), so it is heard as a tape-style bend.
constexpr int FX_TIM      = 0x3D;  // TIM  delay echo time, free scale (00-FF = 0-2 s), global

/** The instrument an INS cell on this step names, or -1. Rightmost wins, as in the scheduler. */
inline int step_ins_instrument(const PhraseStep& s) {
    if (s.fx3Type == FX_INS) return s.fx3Value & 0xFF;
    if (s.fx2Type == FX_INS) return s.fx2Value & 0xFF;
    if (s.fx1Type == FX_INS) return s.fx1Value & 0xFF;
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
        case FX_INS: return "INS"; case FX_TIM: return "TIM";
        case FX_VTR: return "VTR"; case FX_VMV: return "VMV";
        case FX_SCA: return "SCA"; case FX_SCG: return "SCG";
        case FX_AUS: return "AUS"; case FX_AUF: return "AUF";
        case FX_MPG: return "MPG"; case FX_MPB: return "MPB";
        case FX_CCA: return "CCA"; case FX_CCB: return "CCB";
        case FX_CCC: return "CCC"; case FX_CCD: return "CCD";
        default: return "---";
    }
}

// Max parameter byte for an effect. Pool refs (table, groove, EQ preset) and MPG — a 7-bit program
// number — cap at 0x7F; SCA/SCG at 0xBF (twelve keys); the rest at 0xFF. MPB is the 14-bit value's
// top byte, so 0xFF.
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

// The FX-type column's cycle order and the FX picker grid's reading order. An effect missing here
// cannot be typed. A `.ptp` stores the effect CODE and the index is only a live cursor value, so
// re-ordering does not change saved cells — but it does move the picker grid, the recorded input tests, and
// the hidden tails below.
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
    // INS and TIM sit above LPO because the tail is what a release build trims.
    FX_INS,
    FX_TIM,
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

// ⚠️ LPO is hidden from release builds the same way (platform_caps.h `loopWindow`) and must sit
// directly below the MIDI six. Nothing can be dropped from the MIDDLE of this list, so hiding LPO
// also hides MIDI: the trims nest.
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

// ─── Resolved bundle ──────────────────────────────────────────────────────────────────────────────
// An empty optional means "effect not present on this step".
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

/**
 * How many semitones the chain TSP column and the project transpose move THIS step, after the
 * instrument's TRANSP. switch (off wins over any TSX) and the step's TSX.
 * One function because the note, a REPEAT retrigger and the arpeggio all need the same figure.
 * ⚠️ Sliced instruments are deliberately not a case: slice SELECTION already ignores the transpose
 * (voice_derive.h), while the pitch follows it — that is what lets a transposed chain move a break up.
 * `instrumentId` is the raw column; an out-of-range or empty one just transposes.
 */
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
