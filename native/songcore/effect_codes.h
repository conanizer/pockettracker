#ifndef POCKETTRACKER_SONGCORE_EFFECT_CODES_H
#define POCKETTRACKER_SONGCORE_EFFECT_CODES_H

// The FX_* codes and how their bytes read. A leaf: the engine includes it too, so the table engine
// and the scheduler can never disagree on a number.

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
// A CC slot is named by letter; the consumer resolves it to the instrument's controller (event.h
// `CC_SLOT_A`), since an FX-only step's instrument column is not the sounding instrument.
constexpr int FX_MPG      = 0x25;  // MPG  MIDI program change (00-7F)
constexpr int FX_MPB      = 0x26;  // MPB  MIDI pitch bend, absolute (00-FF, 80 = centre)
constexpr int FX_CCA      = 0x27;  // CCA  instrument CC slot A
constexpr int FX_CCB      = 0x28;  // CCB  instrument CC slot B
constexpr int FX_CCC      = 0x29;  // CCC  instrument CC slot C
constexpr int FX_CCD      = 0x2A;  // CCD  instrument CC slot D

// ─── The mixer faders, as effects ─────────────────────────────────────────────────────────────────
// ⚠️ VTR REPLACES the fader (the screen keeps the authored value); `SongcoreHost::stop()` restores it.
constexpr int FX_VTR      = 0x2B;  // VTR  this track's mixer fader (00-FF)
constexpr int FX_VMV      = 0x2C;  // VMV  the master fader (00-FF), global

// ─── The automation pair (automation.h) ───────────────────────────────────────────────────────────
// AUS ramps the effect to its LEFT; AUF carries the destination. Not resolved below — resolution
// discards slot position.
constexpr int FX_AUS      = 0x2D;  // AUS  automation start: xx = curve (00 ease-in, 80 linear, FF ease-out)
constexpr int FX_AUF      = 0x2E;  // AUF  automation finish: xx = destination value

// ─── The instrument filter, per note ──────────────────────────────────────────────────────────────
// They move the instrument's filter and never turn one on. Gone at the next note-on.
constexpr int FX_CUT      = 0x2F;  // CUT  filter cutoff  (00-FF)
constexpr int FX_RES      = 0x30;  // RES  filter resonance (00-FF)

// ─── The scale commands ───────────────────────────────────────────────────────────────────────────
// High nibble = key (0 = C … 11 = B), low = scale slot; capped at 0xBF. Track state, never saved.
constexpr int FX_SCA      = 0x31;  // SCA  this track's scale: x = key, y = slot
constexpr int FX_SCG      = 0x32;  // SCG  every track's scale, same byte layout

// ─── The filter, switched ON from a cell: type AND cutoff ─────────────────────────────────────────
// ⚠️ Both ride ONE bus record (CC id = type, value = cutoff): split, they could land a block apart and
// click at the head of every sweep.
constexpr int FX_LPF      = 0x33;  // LPF  low-pass ON at this cutoff  (00-FF)
constexpr int FX_HPF      = 0x34;  // HPF  high-pass ON at this cutoff (00-FF)
constexpr int FX_BPF      = 0x35;  // BPF  band-pass ON at this cutoff (00-FF)

// ─── The two dirt commands: on the sounding note, reset at the next ───────────────────────────────
// ⚠️ 0x36 is retired and must never be reused.
constexpr int FX_DRV      = 0x37;  // DRV  overdrive amount (00 = clean, FF = heavy)

// One cell, two 4-bit values: left digit crushes bits, right one downsamples.
// ⚠️ Not rampable (automation.h): interpolating the byte wraps the right digit sixteen times.
constexpr int FX_CRU      = 0x38;  // CRU  xy: x = bits crushed (0-F), y = downsample (0-F)

/** The two halves of a CRU byte: bits to crush, and the downsample factor. Both 0 = clean. */
inline constexpr int crush_cmd_bits(int value)       { return (value >> 4) & 0x0F; }
inline constexpr int crush_cmd_downsample(int value) { return value & 0x0F; }

// ─── Fine tune: a semitone either way in 256 steps, on the sounding note ─────────────────────────
constexpr int FX_FIN      = 0x39;  // FIN  fine tune (00 = a semitone flat, 80 = in tune, FF = sharp)

/** A FIN byte in semitones: 80 in tune, 00 a semitone flat, 1/128 per step — meeting PIT with no gap. */
inline constexpr float fine_tune_semitones(int value) { return (value - 128) / 128.0f; }

// Nothing else would catch a centre one step off (0.78 cents) or a wrong divisor.
static_assert(fine_tune_semitones(0x80) == 0.0f, "FIN's centre byte must be exactly in tune");
static_assert(fine_tune_semitones(0x00) == -1.0f, "FIN's low end must be exactly a semitone flat");

// ─── How far a step follows the chain and project transposes ──────────────────────────────────────
// A signed multiplier: 01 normal, 02 double, 00 immune, FF mirrored. Phrase only.
constexpr int FX_TSX      = 0x3A;  // TSX  transpose multiplier (00 immune, 01 normal, FF mirrored)

// ─── The loop window, slid by signed sixteenths of its own length (`10` = one loop) ──────────────
// ⚠️ Cumulative; the engine keeps the total in SIXTEENTHS, since summing rounded offsets would drift.
// Stops at the sample's ends; resets on every new note.
constexpr int FX_LPO      = 0x3B;  // LPO  slide the loop window, signed sixteenths of its own length

/** A LPO byte to signed sixteenths of a loop length: 00-7F forward, 80-FF back. */
inline constexpr int loop_slide_sixteenths(int value) { return (value & 0xFF) < 0x80 ? (value & 0xFF)
                                                                                     : (value & 0xFF) - 256; }

// INS xx: this step's note plays instrument `xx` instead of its I column. Applied after CHA/RND/RNL,
// so they can pick the instrument. Phrase only, and only on a step with a note.
constexpr int FX_INS      = 0x3C;  // INS  play this note on instrument xx

// ─── The delay's echo time, always on the free 0-2 s scale ───────────────────────────────────────
// ⚠️ Replaces the screen's TIME until STOP, like VTR (`MixerHeld`, `SongcoreHost::stop()`).
constexpr int FX_TIM      = 0x3D;  // TIM  delay echo time, free scale (00-FF = 0-2 s), global

// ─── A row for some passes only ───────────────────────────────────────────────────────────────────
// NTH xy: the row plays on pass x of every y entries of its phrase (y = 0: on pass x only); 00 always.
constexpr int FX_NTH      = 0x3E;

inline constexpr bool nth_plays(int value, int pass) {
    const int x = (value >> 4) & 0x0F, y = value & 0x0F;
    if (x == 0 && y == 0) return true;
    if (y == 0) return pass == x;
    return (pass - 1) % y + 1 == x;
}
/** True when no pass can hold the row back: `00`, or one of every one. */
inline constexpr bool nth_always(int value) { return (value & 0xFF) == 0x00 || (value & 0xFF) == 0x11; }

// CHD xy: the note sounds as a chord — x the chord, y how its notes are laid out (chords.h). On a
// phrase row with a note, or a table row in a column that steps once per note; ARP and RPT repeat it.
constexpr int FX_CHD      = 0x3F;

// LEG xx: the row's note does not restart the sound — the note still sounding on the track moves to
// its pitch, at once (00) or sliding over xx ticks. Nothing sounding, or another instrument: a plain
// note. Phrase only.
constexpr int FX_LEG      = 0x40;

// ─── A MACRO instrument's two controls, on the sounding note; the next note starts on the instrument's ──
// What each shapes depends on the model. Any other instrument type ignores them.
constexpr int FX_TMB      = 0x41;  // TMB  TIMBRE (00-FF)
constexpr int FX_CLR      = 0x42;  // CLR  COLOR  (00-FF)

}  // namespace songcore

#endif
