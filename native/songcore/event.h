#ifndef POCKETTRACKER_SONGCORE_EVENT_H
#define POCKETTRACKER_SONGCORE_EVENT_H

// ─── The event schema, as code ──────────────────────────────────────────────────────────────────
//
// Normative companion to docs/internal/event-schema.md (RATIFIED 2026-07-10). One POD record,
// three encodings: (1) the in-memory bus record MidiRouter hands to IMidiConsumers (songcore
// Phase 1+), (2) the conformance-trace text form (below), (3) the JNI packed-int form (Phase 1
// JNI surface). Any change to tags, payload fields, field ORDER, or float derivations is a
// SCHEMA_VERSION bump + regenerated goldens + a doc update, all in one PR.
//
// Everything in a record is an integer. Analog-valued fields are IEEE-754 binary32 carried as
// their raw bit pattern (uint32_t) — trace comparison is exact, no float fuzz. The emitters that
// produce those bits use only +−×÷ on authored bytes (no transcendentals — verified at tap-writing
// 2026-07-10: note→Hz, detune-pow etc. all live BELOW the seam, in the consumers). Songcore
// scheduling TUs compile with no -ffast-math and -ffp-contract=off.
//
// ─── Trace text form (encoding 2) — frozen at tap-writing, 2026-07-10 ───────────────────────────
//
//   # schema=1 sr=44100 tempo=128 mode=render project=<sha1>
//   T PLAY RENDER 00-03            (render: song-row range, hex2)
//   T PLAY SONG 00                 (live: SONG start row / CHAIN id / PHRASE id, hex2)
//   <frame> <track> <instr> <TT> k=v k=v ...
//   T STOP
//
//   frame   decimal, relative to the session start frame latched at T PLAY (render sessions
//           start at 0 naturally; live sessions subtract the transport-start frame so traces
//           are position-independent and device↔host comparable)
//   track   decimal (0-7 sequencer, 8 preview, 255 global)
//   instr   2-digit uppercase hex, or -1 (track-scoped events)
//   TT      type tag, 2-digit uppercase hex
//   k=v     every payload field, always all of them, in the struct order below;
//           ints decimal, floats 0x + exactly 8 uppercase hex digits of the binary32 bits,
//           bools 0/1. Lines end with '\n'. No date/wall-clock anywhere (byte-determinism).
//
//   Canonical comparison: within a PLAY..STOP segment, event lines sort STABLY by
//   (frame, track, rank) — see sortRank() — then the whole file compares byte-for-byte.
//   Relative order of equal-key lines is semantic (FX slots resolve 1→3, last-wins).
//
// ─── JNI packed-int form (encoding 3) — rule frozen now, arrays land with injectEvent() ─────────
//
//   int32 sequence: frameHi, frameLo, track, instrument, type, then payload fields in struct
//   order; floats as raw bits, bools as 0/1. (One rule, no per-event tables.)
//
// ────────────────────────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstring>

namespace songcore {

constexpr int SCHEMA_VERSION = 1;

/**
 * The schema's float encoding, in one place: an analog value is carried as the RAW BITS of its
 * IEEE-754 binary32, never as a float field (§5 / §6 — it is what makes an event byte-comparable
 * across a language boundary at all).
 *
 * `MidiRouter` keeps a private copy of this (`bits()`) because it is the schema's only *emitter*. It
 * is public here because it is no longer the only one: `SongcoreHost::preview_note` builds a NoteOn by
 * hand — a note with no phrase behind it — and a second bit-cast written from memory is a second
 * chance to get the cast wrong.
 *
 * memcpy, not a reinterpret_cast: type-punning through a pointer is UB, and the optimiser is entitled
 * to act on that. (The DECODER, `f32_from_bits`, lives in the generated note_tables.h, which every
 * consumer of the bus already includes.)
 */
inline uint32_t f32_bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

// ─── Envelope ───────────────────────────────────────────────────────────────────────────────────

// track values
constexpr uint8_t TRACK_PREVIEW = 8;    // live/preview lane (PREVIEW_TRACK_ID) — bus-legal, never goldened
constexpr uint8_t TRACK_GLOBAL  = 0xFF; // global events (ExtMasterEq)

// instrument: 0-255 routing key; -1 = none (track-scoped events: NoteOff, CC, EXT)
constexpr int16_t INSTRUMENT_NONE = -1;

// ─── Type tags ──────────────────────────────────────────────────────────────────────────────────
// Events with a MIDI form use their MIDI status high byte; tracker-only events use 0x01-0x7F
// (never valid MIDI status bytes). 0x07-0x7F reserved for future EXT events.

enum EventType : uint8_t {
    EV_EXT_PITCH_RATE = 0x01,  // PBN on empty step (a rate, not an absolute bend)
    EV_EXT_VIBRATO    = 0x02,  // PVB/PVX on empty step (atomic speed+depth pair)
    EV_EXT_TABLE_ROW  = 0x03,  // THO on empty step
    EV_EXT_REVERSE    = 0x04,  // BCK
    EV_EXT_EQ_SLOT    = 0x05,  // EQN
    EV_EXT_MASTER_EQ  = 0x06,  // EQM (track = TRACK_GLOBAL)
    EV_NOTE_OFF       = 0x80,
    EV_NOTE_ON        = 0x90,
    EV_CC             = 0xB0,
    EV_PROGRAM        = 0xC0,  // MPG            (emitter: scheduler.h, MIDI phase D)
    EV_PITCH_BEND     = 0xE0,  // MPB, absolute 14-bit (emitter: scheduler.h, MIDI phase D)
};

// Same-frame drain order (audio-engine.cpp:884/978, pinned as schema law): at equal frame the
// engine applies param-class → NoteOff → NoteOn. Canonical trace sort key is (frame, track, rank).
constexpr int sortRank(uint8_t type) {
    if (type == EV_NOTE_ON)  return 2;
    if (type == EV_NOTE_OFF) return 1;
    return 0;  // CC / EXT / Program / PitchBend all ride the param queue
}

// ─── Payloads ───────────────────────────────────────────────────────────────────────────────────
// Field order below IS the trace k=v order and the JNI packed order. Trace field names are the
// short names in the trailing comments.

// NoteOff modes
constexpr uint8_t NOTE_OFF_RELEASE = 0;  // scheduleNoteOff — KIL soft kill / ADSR release
constexpr uint8_t NOTE_OFF_CUT     = 1;  // scheduleKill / killTrack — declick fade
// ⚠️ **A KEY RELEASE IS NOT A KIL, AND MODE 2 EXISTS BECAUSE THE DIFFERENCE IS AUDIBLE** (MIDI plan
// §4.1, phase E4). KIL means "end this note now, whatever it is": on a one-shot with no release
// envelope it is a declick fade, and that is correct — the user typed a kill. Letting go of a KEY on
// the same instrument must NOT cut it: a drum hit plays out, which is what every sampler with a
// keyboard on it does. The engine call is `scheduleKeyRelease`, and `SamplerVoice::keyRelease` is the
// one place the three-way rule lives (ADSR/TRIG → release, looping → soft kill, one-shot → ignore).
//
// ⚠️ **NO SCHEMA BUMP, AND THAT IS AN ARGUMENT RATHER THAN AN OVERSIGHT.** This adds a VALUE to an
// existing byte field, not a tag, a field or an order — the record's layout is unchanged, and the
// only emitter is `MidiInputRouter` (midi_in.h), which does not ride `MidiRouter` at all. No trace
// can therefore contain a mode 2, and the 36 goldens are byte-identical across this change. The
// moment something the SEQUENCER emits uses it, that stops being true and the rule at the top of
// this file applies in full.
constexpr uint8_t NOTE_OFF_KEY     = 2;  // scheduleKeyRelease — a live key let go of (MIDI in)

// CC ids carried by EV_CC (MIDI plan §6; more ids flow in phase D)
constexpr uint8_t CC_VOLUME      = 7;   // scheduleTrackPhraseVol — the value is the float that
                                        // rides the phraseVol/MOD_SRC_PHRASE_VOL channel:
                                        // instrument vol, or the Vxx byte /255 when Vxx overrides
constexpr uint8_t CC_PAN         = 10;  // scheduleVoicePan   (authored byte /255)
constexpr uint8_t CC_REVERB_SEND = 91;  // scheduleVoiceReverbSend (authored byte /255)
constexpr uint8_t CC_DELAY_SEND  = 93;  // scheduleVoiceDelaySend  (authored byte /255)

// ─── SYMBOLIC CC ids — the instrument's four CC slots (MIDI phase D, plan §6/§8.3) ───────────────
//
// `param` is a uint8_t and a MIDI controller number is seven bits, so 128-255 is a namespace the wire
// can never occupy. 128-131 mean "the controller slot A-D of whatever instrument this track's events
// belong to" — a LETTER, resolved by each consumer against `Instrument::midiCC`, never a number.
//
// ⚠️ **THE RESOLUTION IS CONSUMER-SIDE ON PURPOSE AND THE SCHEDULER MUST NOT DO IT.** A `CCA` on an
// FX-only step has no note beside it, and `PhraseStep::instrument` is 0x00 there whatever is actually
// sounding — resolving at emit time would read slot A of instrument 00 and, since that slot is
// normally unassigned, do NOTHING while looking correct. The instrument a track's events belong to is
// `TrackInstruments` (router.h), the same answer both consumers already use to decide the routing, and
// the same reason it exists: two consumers that resolve differently play a note twice or not at all.
//
// ⚠️ A slot id must never reach a `& 0x7F`: 128 folded that way is CC 0 = BANK SELECT. Every consumer
// resolves or drops it explicitly (midi_out.h `cc_event`, engine_consumer.h `EV_CC`).
constexpr uint8_t CC_SLOT_A = 128, CC_SLOT_B = 129, CC_SLOT_C = 130, CC_SLOT_D = 131;

/** Slot index 0-3 for CC_SLOT_A..D, or -1 for a literal controller number. */
constexpr int cc_slot_index(uint8_t param) {
    return (param >= CC_SLOT_A && param <= CC_SLOT_D) ? param - CC_SLOT_A : -1;
}

// The full NoteOn trigger bundle — the seam args of AudioEngine.scheduleNote, verbatim, tapped at
// entry (after the empty-note guard, before instrument/sample validity checks — invalid-instrument
// and empty-slot NoteOns ARE events; consumers drop them).
//
// Frozen decisions (2026-07-10, doc updated in the same commit):
//  * note = the MIDI number, (octave+1)*12 + pitch. NOT the engine's octave*12+pitch (the doc's
//    draft wording): transpose-coercion can produce octave -1 (MIDI 0-11), which only the MIDI
//    form keeps non-negative. Top-octave authored notes may exceed 127 (B-9 = 131); consumers
//    clamp, the trace records verbatim.
//  * velocity = the seam's midiVelocity: -1 = legacy "derive from velGain" (retrig/arp, IB-19).
//  * velGain rides the seam arg NAMED `volume` and volGain rides `phraseVol` — historically
//    crossed names (PlaybackController.kt, note-queue.h) — the port copies the WIRING.
//  * start = the authored OFF byte (startPointOverride, -1 default). endOffset was REMOVED from
//    the schema: the CUT-slice window is derived below the seam from instrument+note+slice data
//    (instrument-static rule). Same for tableTicRate (always instrument.tableTicRate at the seam).
//  * transpose (chain+song semitones), pit (PIT semitones) and arp (arpeggio offset semitones)
//    ride as THREE separate fields — slice derivation needs chain transpose alone, so folding
//    them would lose information. MIDI out folds all three into data1.
//  * pslDur is in TICKS, pbnRate is the raw FX byte /16 (the seam's tick/step naming discrepancy
//    is documented, values are what they are), vibSpd is Hz ALREADY tempo-scaled, vibDep semitones.
struct NoteOnPayload {
    uint8_t  note;        // note      MIDI number (octave+1)*12+pitch, 0-131 practical
    int8_t   velocity;    // vel       -1 | 0-127 (phrase V column)
    uint32_t velGainBits; // velGain   f32ᵇ (velocity curve, squared) — seam arg `volume`
    uint32_t volGainBits; // volGain   f32ᵇ (instr vol | Vxx/255) — seam arg `phraseVol`
    uint32_t panBits;     // pan       f32ᵇ 0=L ½=C 1=R (PAN-with-note baked here, IB-12)
    int32_t  start;       // start     authored OFF byte 0-255, -1 default
    int32_t  slice;       // slice     SLI index, -1 none
    int32_t  transpose;   // transpose chain+song transpose, semitones
    int32_t  pit;         // pit       PIT offset, semitones
    int32_t  arp;         // arp       arpeggio offset, semitones (retrig grid notes: 0)
    int32_t  tableId;     // tableId   TBL override / retrig continuity, -1 = instrument default
    int32_t  tableRow;    // tableRow  THO-with-note start row 0-15, -1 default
    uint32_t pslOffBits;  // pslOff    f32ᵇ portamento initial offset, semitones
    uint32_t pslDurBits;  // pslDur    f32ᵇ portamento duration, ticks
    uint32_t pbnRateBits; // pbnRate   f32ᵇ raw PBN byte /16, sign = direction
    uint32_t vibSpdBits;  // vibSpd    f32ᵇ Hz, tempo-scaled at emit
    uint32_t vibDepBits;  // vibDep    f32ᵇ semitones
};

struct NoteOffPayload  { uint8_t mode; };                          // mode      NOTE_OFF_*
struct CcPayload       { uint8_t param; uint32_t valueBits; };     // param value
struct ProgramPayload  { uint8_t program; };                       // program   0-127
// value14: the authored MPB byte shifted into the top eight bits of the 14-bit range (`b << 6`), so
// 0x80 lands EXACTLY on centre 0x2000 and 0x00 on 0. The cost is the very top: 0xFF gives 16320 of a
// possible 16383, 0.4% short of full bend. Centre being exact is worth more than that 0.4% — a bend
// that does not rest at zero detunes every note that is not bending.
struct PitchBendPayload{ uint16_t value14; };                      // value     centre 0x2000
struct ExtPitchRatePayload { uint32_t rateBits; uint16_t tempo; }; // rate tempo (rate: raw byte /16)
struct ExtVibratoPayload   { uint32_t speedBits, depthBits; };     // speed depth
struct ExtTableRowPayload  { uint8_t row; };                       // row       0-15
struct ExtReversePayload   { uint8_t reverse, restart; };          // reverse restart
struct ExtEqSlotPayload    { int16_t slot; };                      // slot      -1 bypass | 0-127
struct ExtMasterEqPayload  { int16_t slot; };                      // slot      -1 bypass | 0-127

// ─── The record ─────────────────────────────────────────────────────────────────────────────────

struct Event {
    int64_t frame;       // absolute frames at session sample rate, relative to session start
    uint8_t track;       // 0-7 | TRACK_PREVIEW | TRACK_GLOBAL
    int16_t instrument;  // 0-255 routing key | INSTRUMENT_NONE
    uint8_t type;        // EventType

    union {
        NoteOnPayload       noteOn;
        NoteOffPayload      noteOff;
        CcPayload           cc;
        ProgramPayload      program;
        PitchBendPayload    pitchBend;
        ExtPitchRatePayload extPitchRate;
        ExtVibratoPayload   extVibrato;
        ExtTableRowPayload  extTableRow;
        ExtReversePayload   extReverse;
        ExtEqSlotPayload    extEqSlot;
        ExtMasterEqPayload  extMasterEq;
    };
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_EVENT_H
