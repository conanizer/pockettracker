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

// ─── MIDI phase D — the commands that speak the CC map (plan §8.3) ────────────────────────────────
//
// ⚠️ **THESE ARE ROUTER EVENTS, NOT "EXTERNAL-ONLY EFFECTS".** They resolve to the same three bus
// records every other consumer already reads (EV_PROGRAM / EV_PITCH_BEND / EV_CC), so `CCA` on a
// SAMPLER instrument moves the mapped engine param and `CCA` on an EXTERNAL one puts a controller on
// the wire — one command vocabulary across module types, which is the whole payoff of the seam.
//
// ⚠️ A CC slot is named by its LETTER, never by a controller number: the number lives in the
// instrument (`Instrument::midiCC[slot].cc`), so the same phrase follows an instrument that has been
// re-patched. The letter→number resolution happens in the CONSUMER (event.h `CC_SLOT_A`), for the
// reason `TrackInstruments` exists at all — the step's instrument COLUMN is 0x00 on an FX-only step
// and is not the instrument that is sounding.
constexpr int FX_MPG      = 0x25;  // MPG  MIDI program change (00-7F)
constexpr int FX_MPB      = 0x26;  // MPB  MIDI pitch bend, absolute (00-FF, 80 = centre)
constexpr int FX_CCA      = 0x27;  // CCA  instrument CC slot A
constexpr int FX_CCB      = 0x28;  // CCB  instrument CC slot B
constexpr int FX_CCC      = 0x29;  // CCC  instrument CC slot C
constexpr int FX_CCD      = 0x2A;  // CCD  instrument CC slot D

// ─── The mixer faders, as effects ─────────────────────────────────────────────────────────────────
//
// ⚠️ **VTR REPLACES THE TRACK'S FADER, IT DOES NOT SCALE IT** — the same relationship Vxx has with the
// instrument volume, and the reason is that one byte cannot carry both. The MIXER screen keeps drawing
// the AUTHORED value throughout: the number you are editing must be the number you typed, so the fader
// on screen is where the song STARTS and VTR is where the song has moved it to.
//
// ⚠️ Which makes the restore on stop() load-bearing rather than tidy. These write engine state that no
// later event puts back, so without it a song that fades out leaves the engine at the faded level, the
// MIXER lies about it, and the next PLAY starts quiet. `Sequencer::mixer_vol_active()` is the flag and
// `SongcoreHost::stop()` is the restore — the shape `eqmActive_` already uses, for the same reason.
constexpr int FX_VTR      = 0x2B;  // VTR  this track's mixer fader (00-FF)
constexpr int FX_VMV      = 0x2C;  // VMV  the master fader (00-FF), global

// ─── The automation pair ──────────────────────────────────────────────────────────────────────────
//
// AUS opens a ramp on the effect in the slot to its LEFT and carries the CURVE; AUF, on a later step
// of the same phrase and track, carries the DESTINATION. The pairing rule, which parameters can be
// ramped and why, all live in `automation.h` — this file only names the two codes.
//
// ⚠️ **NEITHER APPEARS IN `resolve_step_params` BELOW, AND THAT IS THE DESIGN RATHER THAN AN
// OMISSION.** Resolution folds three slots into one bundle last-wins and throws the slot NUMBER away.
// AUS's whole meaning is positional — "the automatable effect nearest to my left" — so a resolved
// bundle cannot express it, and giving it a field would be a field whose value could not answer the
// question. Pairing reads the step's slots directly (`find_ramps`), and both codes fall through
// resolution's default no-op arm exactly as ARP / CHA / RND do.
constexpr int FX_AUS      = 0x2D;  // AUS  automation start: xx = curve (00 ease-in, 80 linear, FF ease-out)
constexpr int FX_AUF      = 0x2E;  // AUF  automation finish: xx = destination value

// ─── The instrument filter, per note ──────────────────────────────────────────────────────────────
//
// ⚠️ **BOTH MOVE THE FILTER THE INSTRUMENT DECLARES, AND NEITHER TURNS ONE ON.** They write the
// cutoff/resonance of the voice's own SVF, which an instrument with FILTER TYPE = OFF does not run at
// all — so on such a note they are inert rather than switching on a type the author never typed.
// The value is the same 00-FF byte the INSTRUMENT screen's FREQ and RES cells hold.
//
// Per-note by construction rather than by a restore: a note-on reloads the whole chain from the
// instrument (`SamplerVoice::triggerNote`), so the next note starts at the instrument's own values
// with nothing to put back.
constexpr int FX_CUT      = 0x2F;  // CUT  filter cutoff  (00-FF)
constexpr int FX_RES      = 0x30;  // RES  filter resonance (00-FF)

// ─── The scale commands ───────────────────────────────────────────────────────────────────────────
//
// One byte, two nibbles: the high one is the KEY (0 = C … 11 = B) and the low one is the SLOT in the
// project's 16-scale pool. `effect_value_max` caps the pair at 0xBF so the key nibble cannot be typed
// past B — keys 12..15 do not exist, and a cell whose top half selects nothing is a cell that reads
// as broken.
//
// ⚠️ **NEITHER IS SONG STATE — the scale a track is in lives on `TrackState`, exactly as the groove
// does.** The project stores the POOL and the default key; which slot a track has been moved to is a
// property of the take, so it comes back with a LIVE rewind (`Checkpoint::trackState`) and it is gone
// at STOP. Nothing is written to the `.ptp`, and nothing needs to be.
//
// SCG is SCA applied to all eight tracks rather than a second, song-level variable. That is what
// makes it a way BACK: a track moved by SCA has no other route to the song's scale, and a global that
// sat beside the per-track override would leave that track wherever its last SCA put it.
constexpr int FX_SCA      = 0x31;  // SCA  this track's scale: x = key, y = slot
constexpr int FX_SCG      = 0x32;  // SCG  every track's scale, same byte layout

// ─── The filter, switched ON from a cell ──────────────────────────────────────────────────────────
//
// ⚠️ **EACH ONE CARRIES A TYPE AND A CUTOFF TOGETHER, which is exactly what CUT and RES do not.** CUT
// moves the filter the instrument declares; these three declare it. So an instrument whose FILTER TYPE
// is OFF — the state a fresh instrument is in — can be given a filter from the FX column, and CUT/RES
// become reachable on it without a trip to the INSTRUMENT screen first.
//
// ⚠️ **THE TYPE AND THE CUTOFF MUST LAND ON THE SAME FRAME.** They ride ONE bus record — the CC id
// carries the type, its value carries the cutoff — rather than two, because a filter that opens one
// block before it changes shape is an audible click at the head of every sweep.
//
// ⚠️ Three commands, three filters, and NOT a set to widen: the SVF also computes notch and peak, and
// those two are reachable from nowhere at all today. Giving them a door is a one-line change to the
// INSTRUMENT screen's type list, never a fourth and fifth command here.
//
// CUT keeps a job beside them: it moves the cutoff WITHOUT touching the type, which is what a sweep
// over a filter that is already the right one needs.
constexpr int FX_LPF      = 0x33;  // LPF  low-pass ON at this cutoff  (00-FF)
constexpr int FX_HPF      = 0x34;  // HPF  high-pass ON at this cutoff (00-FF)
constexpr int FX_BPF      = 0x35;  // BPF  band-pass ON at this cutoff (00-FF)

/** The FilterModule type an LPF/HPF/BPF cell turns on (1 lp | 2 hp | 3 bp), or 0 for anything else. */
inline constexpr int fx_filter_mode(int code) {
    return code == FX_LPF ? 1 : code == FX_HPF ? 2 : code == FX_BPF ? 3 : 0;
}

// ─── The two dirt commands ────────────────────────────────────────────────
//
// Both write a value the sampler's per-block recompute already reads, so each takes effect while
// the note is still sounding, and each is gone at the next note-on — the same per-note lifetime
// CUT and RES have, and for the same reason: a trigger reseeds them from the instrument.
//
// ⚠️ **0x36 IS RETIRED AND MUST NEVER BE REUSED.** It was a sample-end command, cut before it
// ever shipped: KIL's latency byte already ends a note where the writer wants it, and two ways to
// stop a note early is one too many. A code is a saved cell's identity, so a project written while
// it existed must not read back as something else.
constexpr int FX_DRV      = 0x37;  // DRV  overdrive amount (00 = clean, FF = heavy)

// ⚠️ **ONE CELL, TWO INDEPENDENT 4-BIT VALUES** — the left digit crushes bits, the right one drops the
// sample rate. They are two different kinds of grit, both already 4-bit at the module, so one cell
// carries both rather than two effect codes carrying half-empty bytes each.
//
// ⚠️ **AND THAT IS WHY CRU IS NOT IN THE RAMP REGISTRY.** It would pass the full-range assert — the
// byte really does span 00-FF — but interpolating it sweeps the left digit while the right one wraps
// F→0 sixteen times on the way. The byte is not a quantity, so a ramp over it is not a sweep.
constexpr int FX_CRU      = 0x38;  // CRU  xy: x = bits crushed (0-F), y = downsample (0-F)

/** The two halves of a CRU byte: bits to crush, and the downsample factor. Both 0 = clean. */
inline constexpr int crush_cmd_bits(int value)       { return (value >> 4) & 0x0F; }
inline constexpr int crush_cmd_downsample(int value) { return value & 0x0F; }

// ─── The cents between the semitones ─────────────────────────────────────────────────────────────
//
// PIT moves a note in whole semitones; this fills the gap, one semitone either way in 256 steps of
// 0.78 cents. The two therefore meet with no interval unreachable between them.
//
// ⚠️ **IT RETUNES THE NOTE THAT IS ALREADY SOUNDING**, which is what makes a ramp over it a pitch
// bend of up to a semitone rather than a setting for the next note. On the step that also carries a
// note it lands one frame behind the trigger (`voiceFxFrame`, scheduler.h), so the note it tunes is
// that one — the same ordering CUT, RES and the sends all rely on.
//
// ⚠️ Per-note like those: a trigger reseeds the parameter bus, so the tuning is gone at the next note
// with nothing to restore.
constexpr int FX_FIN      = 0x39;  // FIN  fine tune (00 = a semitone flat, 80 = in tune, FF = sharp)

// ─── How far a step follows the transposes ───────────────────────────────────────────────────────
//
// A SIGNED MULTIPLIER over the transpose the step would otherwise get: 01 normal, 02 twice as far,
// 00 immune, FF the same distance the other way, FE twice as far the other way. Every product is a
// whole number of semitones, so no note can land between two keys and no rounding rule exists to
// disagree about — which is why the multiplier is signed rather than the divisor the request first
// asked for.
//
// ⚠️ **IT SCALES THE CHAIN TSP COLUMN AND THE PROJECT TRANSPOSE TOGETHER**, because those are the
// one quantity the scheduler carries and the same one the instrument's TRANSP. switch silences. A
// command that moved with one and not the other would be a note whose pitch depends on which of two
// screens the transpose was typed on.
//
// ⚠️ It is a PHRASE command with no table arm: a table row runs after its note has already been
// scheduled, and by then the transpose is folded into the pitch and gone. The table-side want the
// request describes — a drum kit that ignores the song transpose — is the instrument's TRANSP.
// switch (model.h), which is read at the same site.
constexpr int FX_TSX      = 0x3A;  // TSX  transpose multiplier (00 immune, 01 normal, FF mirrored)

// ─── The loop window, slid ───────────────────────────────────────────────────────────────────────
//
// ⭐⭐ **IT MOVES BOTH ENDS OF THE LOOP TOGETHER, SO THE LOOP'S LENGTH NEVER CHANGES.** That is the
// whole command: on a looped note the loop length is what sets the pitch, so a slider that preserved
// only one end would be a pitch control. What changes is which part of the sample the loop is
// reading — the note holds still and its tone moves.
//
// ⚠️ **THE UNIT IS SIXTEENTHS OF THE LOOP'S OWN LENGTH, SIGNED.** `10` is one whole loop, `01` a
// sixteenth of one, `F0` a whole loop backwards. It is relative to the loop rather than to the file,
// so one cell value does the same musical thing on a short loop and a long one and nobody has to
// count samples. LGPT's equivalent counts raw samples in a 16-bit value; one byte cannot.
//
// ⚠️⚠️ **IT IS CUMULATIVE, AND THE RUNNING TOTAL IS KEPT IN SIXTEENTHS RATHER THAN IN SAMPLES.**
// A loop whose length is not a multiple of 16 makes every step round, and sixteen rounded steps do
// NOT add up to one loop — on a sample of back-to-back single-cycle waves that is a wave boundary
// drifting further out with each step, silent and cumulative. The engine therefore keeps the count
// and derives the sample offset from the TOTAL each block, so sixteen steps of `01` land exactly
// where one step of `10` lands, always.
//
// ⚠️ The window STOPS at each end of the sample rather than wrapping — a wrap makes a drone jump —
// and the count resets on every new note, so one cell colours that note and the next starts clean.
//
// ⭐ `00` is safely "no movement": the command is relative-only, so a zero step has an obvious
// meaning and punches no hole in the range the way LGPT's absolute play-head command does.
constexpr int FX_LPO      = 0x3B;  // LPO  slide the loop window, signed sixteenths of its own length

/** A LPO byte to signed sixteenths of a loop length: 00-7F forward, 80-FF back. */
inline constexpr int loop_slide_sixteenths(int value) { return (value & 0xFF) < 0x80 ? (value & 0xFF)
                                                                                     : (value & 0xFF) - 256; }

/** Slot index 0-3 for FX_CCA..FX_CCD, or -1 for any other effect code. */
inline int fx_cc_slot(int code) {
    return (code >= FX_CCA && code <= FX_CCD) ? code - FX_CCA : -1;
}

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
        case FX_CUT: return "CUT"; case FX_RES: return "RES";
        case FX_LPF: return "LPF"; case FX_HPF: return "HPF"; case FX_BPF: return "BPF";
        case FX_DRV: return "DRV"; case FX_CRU: return "CRU";
        case FX_FIN: return "FIN"; case FX_TSX: return "TSX"; case FX_LPO: return "LPO";
        case FX_VTR: return "VTR"; case FX_VMV: return "VMV";
        case FX_SCA: return "SCA"; case FX_SCG: return "SCG";
        case FX_AUS: return "AUS"; case FX_AUF: return "AUF";
        case FX_MPG: return "MPG"; case FX_MPB: return "MPB";
        case FX_CCA: return "CCA"; case FX_CCB: return "CCB";
        case FX_CCC: return "CCC"; case FX_CCD: return "CCD";
        default: return "---";
    }
}

// Max parameter byte for an effect: table/groove/EQ-preset pool refs cap at 0x7F (128 slots), all
// others use the full 0xFF range. Mirrors effectValueMax().
//
// MPG joins the 0x7F group and does not merely follow it: a MIDI program number IS seven bits, so a
// cell that let you type FF would be a cell whose top half sends something else (`& 0x7F` folds 0x80
// back onto program 0). MPB stays 0xFF — it is the wide 14-bit value's top byte, not a 7-bit one.
//
// SCA/SCG stop at 0xBF for the same kind of reason and a different arithmetic: their high nibble is a
// KEY, and there are twelve of those, so 0xC0 and up name a key that does not exist.
inline constexpr int effect_value_max(int effect_type) {
    if (effect_type == FX_SCA || effect_type == FX_SCG) return 0xBF;
    return (effect_type == FX_TBL || effect_type == FX_GRV ||
            effect_type == FX_EQN || effect_type == FX_EQM ||
            effect_type == FX_MPG) ? 127 : 255;
}

/**
 * The two halves of an SCA/SCG byte. Written here, beside the codes, because the editor's cap, the
 * scheduler's application and every check have to agree about which nibble is which — and a file
 * written by hand can carry a key nibble the editor refuses, so the key is clamped rather than
 * trusted. The slot needs no clamp: four bits is exactly the pool.
 */
inline constexpr int scale_cmd_key(int value) { return ((value >> 4) & 0x0F) > 11 ? 11 : ((value >> 4) & 0x0F); }
inline constexpr int scale_cmd_slot(int value) { return value & 0x0F; }

// The cycle order of the FX-type column, and the reading order of the FX helper grid. This is a
// UI-facing list (an FX column stores an INDEX into it, and A+RIGHT steps that index), but it lives here
// beside the codes and the names because those three must never drift: an entry added to the effects
// without an entry here is an effect no one can type.
//
// ⚠️ INSERTING HERE IS SAFE AND APPENDING A CODE IS THE RULE — they are different lists. A `.ptp`
// stores the effect CODE (`PhraseStep::fxNType`), and the index is only ever a live cursor value
// (cursor.h converts to it, phrase_editor.cpp converts back), so re-ordering this array cannot change
// what a saved cell means. What it DOES move is the picker's grid and `ptinput`'s navigation golden.
inline constexpr int EFFECT_TYPES[] = {
    FX_NONE, FX_ARC, FX_CHA, FX_LAT, FX_GRV, FX_HOP, FX_TIC, FX_ARPEGGIO, FX_KILL, FX_OFFSET,
    FX_RND, FX_RNL, FX_REPEAT, FX_TBL, FX_THO, FX_VOLUME,
    FX_PSL, FX_PBN, FX_PVB, FX_PVX, FX_PIT, FX_SLI,
    FX_PAN, FX_BCK, FX_RSEND, FX_DSEND, FX_EQN, FX_EQM,
    // The mixer faders, beside the other things that act on the bus rather than the note
    FX_VTR, FX_VMV,
    // The automation pair — they act on another slot, not on a bus
    FX_AUS, FX_AUF,
    // ⚠️ CUT/RES SIT AT THE END OF THE NON-MIDI EFFECTS RATHER THAN BESIDE BCK AND THE SENDS, WHERE
    // THEY BELONG BY SUBJECT. Appending leaves every index below them naming the effect it always
    // named; inserting would renumber a third of the list, and `ptinput`'s Kotlin-recorded EDIT cases
    // carry the resulting CELL BYTE — which is the effect CODE at an index. Their golden has no
    // independent author left to re-record it (tools/ptinput/main.cpp), so a renumber there would be
    // certified by nothing but the code that caused it.
    FX_CUT, FX_RES,
    // The scale commands, appended for the same reason CUT/RES were, and ahead of the MIDI six
    // because the assert below requires those to stay last. Appending renumbers everything after it,
    // and what that costs is `ptinput`'s golden and nothing else: an FX cell holds an INDEX only
    // while it is a live cursor value, and a `.ptp` holds the effect CODE.
    FX_SCA, FX_SCG,
    // The three that switch a filter ON. Appended for the reason CUT/RES and the scale pair were,
    // and reading right after them because that is where someone looking for the filter will look.
    FX_LPF, FX_HPF, FX_BPF,
    // The two dirt commands. Appended for the same reason.
    FX_DRV, FX_CRU,
    // Fine tune, next to the pitch commands by subject and here by the same appending rule.
    FX_FIN,
    // The transpose multiplier, appended for the same reason. It sits after FIN rather than beside
    // PIT because a code's position here is the picker's grid, and the grid is never re-ordered.
    FX_TSX,
    // The loop-window slider, appended for the same reason.
    FX_LPO,
    // The MIDI commands (see the static_assert below — they must stay the LAST six)
    FX_MPG, FX_MPB, FX_CCA, FX_CCB, FX_CCC, FX_CCD,
};
inline constexpr int EFFECT_TYPE_COUNT = static_cast<int>(sizeof(EFFECT_TYPES) / sizeof(int));

// ⚠️ THE SIX MIDI COMMANDS ARE THE LAST SIX ENTRIES, AND A BUILD THAT HIDES THEM RELIES ON IT.
// A build without the MIDI surfaces (native/ui/platform_caps.h `midi`) shows the first
// EFFECT_TYPE_COUNT_NO_MIDI entries and nothing else. That is only safe because they are a TAIL: an
// FX cell stores an INDEX into this array while the .ptp stores the effect CODE, so shortening the
// list leaves every index 0..EFFECT_TYPE_COUNT_NO_MIDI-1 naming the effect it always named. Move one
// of them, or append a non-MIDI effect after them, and the two builds disagree about what a saved
// cell means — hence the assert rather than a comment.
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

// ⚠️ `LPO` IS HIDDEN THE SAME WAY, AND IT IS THE ENTRY DIRECTLY BELOW THE MIDI SIX.
// The loop-window slide and the OSCILLATOR loop mode it belongs with are held back from release
// builds (native/ui/platform_caps.h `loopWindow`) while the shape of the command is still open. The
// trick is the MIDI one and it works for the same reason — a TAIL can be trimmed without renaming
// any remaining index, and a .ptp stores the CODE, so a cell typed in a build that has it reads back
// as `LPO` in one that does not. ⚠️ **THERE IS NO WAY TO DROP AN ENTRY FROM THE MIDDLE OF THIS LIST**,
// so hiding `LPO` requires the MIDI six to be hidden as well; the two trims nest, they do not
// combine freely.
inline constexpr int PREVIEW_EFFECT_COUNT     = 1;   // FX_LPO
inline constexpr int EFFECT_TYPE_COUNT_STABLE = EFFECT_TYPE_COUNT_NO_MIDI - PREVIEW_EFFECT_COUNT;

static_assert(EFFECT_TYPES[EFFECT_TYPE_COUNT_STABLE] == FX_LPO,
              "LPO must sit directly below the MIDI six — a build that hides it shortens the list, "
              "so an effect moved after it would be hidden too");

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
    std::optional<int> filterCutValue;    // CUT
    std::optional<int> filterResValue;    // RES
    // LPF / HPF / BPF. ONE optional for all three, because what they emit is one write: the byte is
    // the cutoff and `filterModeType` is the filter it turns on. Two of them on the same step then
    // behave like every other pair in this bundle — last of slots 1→3 wins — instead of leaving the
    // type from one and the cutoff from the other.
    std::optional<int> filterModeValue;   // LPF/HPF/BPF cutoff byte
    int filterModeType = 0;               // 1 lp | 2 hp | 3 bp; meaningless unless the above is set
    std::optional<int> driveValue;        // DRV
    std::optional<int> crushValue;        // CRU — both nibbles still packed, split at the engine
    std::optional<int> fineTuneValue;     // FIN — authored byte, turned into semitones at the engine
    std::optional<int> tsxMultiplier;     // TSX — already decoded to a signed multiplier
    std::optional<int> loopSlideValue;    // LPO — already decoded to signed sixteenths of a loop
    std::optional<int> eqnSlot;
    std::optional<int> eqmSlot;
    // SCA / SCG: the authored byte, key and slot nibbles still packed. It is split at the point of
    // APPLICATION (scale_cmd_key / scale_cmd_slot) rather than here, so the bundle carries what the
    // cell holds and the clamp on a hand-written key lives in one place.
    std::optional<int> scaleTrackByte;   // SCA
    std::optional<int> scaleGlobalByte;  // SCG
    std::optional<int> trackVolValue;   // VTR (authored byte)
    std::optional<int> masterVolValue;  // VMV (authored byte)
    // MIDI phase D. `ccSlotValue[i]` is slot A..D's authored 00-FF byte; the controller NUMBER it
    // moves is the instrument's, resolved consumer-side (see the FX_CCA note above).
    std::optional<int> midiProgram;                    // MPG
    std::optional<int> midiBend;                       // MPB (authored byte, not the 14-bit value)
    std::optional<int> ccSlotValue[MIDI_CC_SLOTS];     // CCA-CCD
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
            case FX_CUT:    p.filterCutValue = value; break;
            case FX_RES:    p.filterResValue = value; break;
            case FX_LPF: case FX_HPF: case FX_BPF:
                p.filterModeValue = value;
                p.filterModeType  = fx_filter_mode(type);
                break;
            case FX_DRV:    p.driveValue     = value; break;
            case FX_CRU:    p.crushValue     = value; break;
            case FX_FIN:    p.fineTuneValue  = value; break;
            // Decoded here, the way PIT is, so the scheduler multiplies by a number rather than by
            // a byte it would have to remember to sign-extend.
            case FX_TSX:    p.tsxMultiplier = (value < 0x80) ? value : value - 256; break;
            // Decoded here for the same reason TSX and PIT are.
            case FX_LPO:    p.loopSlideValue = loop_slide_sixteenths(value); break;
            case FX_EQN:    p.eqnSlot = value; break;
            case FX_EQM:    p.eqmSlot = value; break;
            case FX_SCA:    p.scaleTrackByte = value; break;
            case FX_SCG:    p.scaleGlobalByte = value; break;
            case FX_VTR:    p.trackVolValue = value; break;
            case FX_VMV:    p.masterVolValue = value; break;
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
            // ARP / CHA / RND / RNL / TIC and any unknown code: no resolved field — no-op.
            default: break;
        }
    }
    return p;
}

/**
 * How many semitones the chain TSP column and the project transpose actually move THIS step, after
 * the instrument's TRANSP. switch and the step's own TSX have had their say.
 *
 * ⭐ **ONE FUNCTION BECAUSE THE SCHEDULER TRANSPOSES A STEP IN THREE PLACES** — the note, a REPEAT
 * retrigger and the arpeggio all read the same figure, and a fourth site added later reads it too.
 * The alternative is three call sites each remembering to ask the same two questions.
 *
 * **TRANSP. off wins over any TSX.** An instrument that does not follow transposition at all is not
 * asking to be moved twice as far; the switch is a property of the sound, the command a property of
 * one step.
 *
 * ⚠️ **A SLICED INSTRUMENT IS NOT A CASE HERE, AND MUST NOT BECOME ONE.** Its slice *selection* is
 * already immune — `voice_derive.h` picks the slice from `note − transpose`, the raw phrase note —
 * while its *pitch* deliberately follows, which is what lets a transposed chain move a chopped break
 * up an octave. Refusing the transpose here would silently take that away. The guard `model.h`
 * asks for around slices is against playing a DIFFERENT SLICE, and that guard is already in place.
 *
 * `instrumentId` is the step's raw instrument column, not a clamped index: an out-of-range or empty
 * column asks no instrument's opinion and transposes exactly as it always did.
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
