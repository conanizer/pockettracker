#ifndef POCKETTRACKER_SONGCORE_EVENT_H
#define POCKETTRACKER_SONGCORE_EVENT_H

// ─── The event schema, as code ──────────────────────────────────────────────────────────────────
//
// One POD record: the bus record MidiRouter hands to every IMidiConsumer, and the conformance-trace
// line below. ⚠️ Changing a tag, a payload field, field ORDER or a float derivation is a
// SCHEMA_VERSION bump and regenerated goldens.
//
// Everything is an integer; analog values are IEEE-754 binary32 carried as raw bits, so traces
// compare exactly. Emitters use only +−×÷ on authored bytes — note→Hz and anything transcendental
// lives below the seam — and the scheduling TUs build without fast-math and with -ffp-contract=off.
//
// ─── Trace text form ────────────────────────────────────────────────────────────────────────────
//
//   # schema=1 sr=44100 tempo=128 mode=render project=<sha1>
//   T PLAY RENDER 00-03            (render: song-row range, hex2)
//   T PLAY SONG 00                 (live: SONG start row / CHAIN id / PHRASE id, hex2)
//   <frame> <track> <instr> <TT> k=v k=v ...
//   T STOP
//
//   frame   decimal, relative to the frame latched at T PLAY, so traces are position-independent
//   track   decimal (0-7 sequencer, 8 preview, 255 global)
//   instr   2-digit uppercase hex, or -1 (track-scoped events)
//   TT      type tag, 2-digit uppercase hex
//   k=v     every payload field, in struct order; ints decimal, floats 0x + 8 uppercase hex digits
//           of the binary32 bits, bools 0/1. No wall-clock anywhere (byte-determinism).
//
//   Comparison: within a PLAY..STOP segment, lines sort STABLY by (frame, track, sortRank), then the
//   files compare byte for byte. Order among equal keys is semantic (FX slots resolve 1→3, last-wins).

#include <cmath>
#include <cstdint>
#include <cstring>

namespace songcore {

constexpr int SCHEMA_VERSION = 2;

/**
 * An analog value as the raw bits of its binary32. memcpy, not a reinterpret_cast: type-punning
 * through a pointer is UB. Public because `SongcoreHost::preview_note` also builds a NoteOn by hand.
 * The decoder, `f32_from_bits`, is in the generated note_tables.h.
 */
inline uint32_t f32_bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

// ─── Envelope ───────────────────────────────────────────────────────────────────────────────────

// track values
constexpr uint8_t TRACK_PREVIEW = 8;    // live/preview lane — bus-legal, never goldened
constexpr uint8_t TRACK_GLOBAL  = 0xFF; // global events (ExtMasterEq)

// instrument: 0-255 routing key; -1 = none (track-scoped events: NoteOff, CC, EXT)
constexpr int16_t INSTRUMENT_NONE = -1;

// ─── Type tags ──────────────────────────────────────────────────────────────────────────────────
// Events with a MIDI form use their MIDI status byte; tracker-only events use 0x01-0x7F, which are
// never MIDI status bytes.

enum EventType : uint8_t {
    EV_EXT_PITCH_RATE = 0x01,  // PBN on empty step (a rate, not an absolute bend)
    EV_EXT_VIBRATO    = 0x02,  // PVB/PVX on empty step (atomic speed+depth pair)
    EV_EXT_TABLE_ROW  = 0x03,  // THO on empty step
    EV_EXT_REVERSE    = 0x04,  // BCK
    EV_EXT_EQ_SLOT    = 0x05,  // EQN
    EV_EXT_MASTER_EQ  = 0x06,  // EQM (track = TRACK_GLOBAL)
    // ⚠️ A morph carries BAND VALUES, not a slot — it is how the EQ reaches a setting no preset holds,
    // so it cannot be folded into the two above.
    EV_EXT_EQ_MORPH        = 0x07,  // an AUS/AUF tick over EQN
    EV_EXT_MASTER_EQ_MORPH = 0x08,  // an AUS/AUF tick over EQM (track = TRACK_GLOBAL)
    EV_NOTE_OFF       = 0x80,
    EV_NOTE_ON        = 0x90,
    EV_CC             = 0xB0,
    EV_PROGRAM        = 0xC0,  // MPG
    EV_PITCH_BEND     = 0xE0,  // MPB, absolute 14-bit
};

// At equal frame the engine applies param-class → NoteOff → NoteOn (processAudioBlock). The trace sort
// key is (frame, track, rank).
constexpr int sortRank(uint8_t type) {
    if (type == EV_NOTE_ON)  return 2;
    if (type == EV_NOTE_OFF) return 1;
    return 0;  // CC / EXT / Program / PitchBend all ride the param queue
}

// ─── Payloads ───────────────────────────────────────────────────────────────────────────────────
// Field order below IS the trace k=v order; trace field names are in the trailing comments.

// NoteOff modes
constexpr uint8_t NOTE_OFF_RELEASE = 0;  // scheduleNoteOff — KIL soft kill / ADSR release
constexpr uint8_t NOTE_OFF_CUT     = 1;  // scheduleKill / killTrack — declick fade
// ⚠️ A key release is not a KIL. KIL ends the note (a declick fade on a one-shot); letting go of a
// KEY lets a drum hit play out. `SamplerVoice::keyRelease` holds the rule (ADSR/TRIG → release,
// looping → soft kill, one-shot → ignore).
// Only MIDI input emits mode 2, and it does not ride `MidiRouter`, so no trace holds one. The day the
// sequencer emits it, the schema rule at the top applies.
constexpr uint8_t NOTE_OFF_KEY     = 2;  // scheduleKeyRelease — a live key let go of (MIDI in)

// CC ids carried by EV_CC
constexpr uint8_t CC_VOLUME      = 7;   // scheduleTrackPhraseVol — the phraseVol channel: instrument
                                        // vol, or the Vxx byte /255 when Vxx overrides
constexpr uint8_t CC_PAN         = 10;  // scheduleVoicePan   (authored byte /255)
constexpr uint8_t CC_REVERB_SEND = 91;  // scheduleVoiceReverbSend (authored byte /255)
constexpr uint8_t CC_DELAY_SEND  = 93;  // scheduleVoiceDelaySend  (authored byte /255)
// CUT / RES: the standard cutoff (74) and resonance (71) controllers, so the same cell sweeps this
// engine's filter and an EXTERNAL synth's.
constexpr uint8_t CC_FILTER_CUT  = 74;  // scheduleVoiceFilterCut (authored byte /255)
constexpr uint8_t CC_FILTER_RES  = 71;  // scheduleVoiceFilterRes (authored byte /255)

// ─── Symbolic CC ids — the instrument's four CC slots ────────────────────────────────────────────
//
// A MIDI controller is seven bits, so 128-255 can never come off the wire. 128-131 mean "slot A-D of
// the instrument this track is playing", resolved by each consumer against `Instrument::midiCC`.
// ⚠️ Never resolved by the scheduler: on an FX-only step `PhraseStep::instrument` is 0x00 whatever is
// sounding. Consumers use `TrackInstruments` (router.h), the same answer the routing gate uses.
// ⚠️ A slot id must never reach a `& 0x7F`: 128 would become CC 0, BANK SELECT.
constexpr uint8_t CC_SLOT_A = 128, CC_SLOT_B = 129, CC_SLOT_C = 130, CC_SLOT_D = 131;

// ─── Engine-only CC ids ──────────────────────────────────────────────────────────────────────────
//
// Ids above 127 that are not slots have no MIDI controller. `resolve_cc_param` (model.h) maps them to
// −1, so `midi_out.h` drops them, while `engine_consumer.h` matches them as literals.
// ⚠️ None may ever reach a `& 0x7F`: each would alias a real controller (132 → CC 4, 135 → CC 7, …).
//
// The mixer faders (VTR / VMV).
// ⚠️ CC_MASTER_VOL rides TRACK_GLOBAL: track-scoped, the EXTERNAL-routing gate would swallow it on a
// MIDI track. CC_TRACK_VOL is genuinely track-scoped and IS gated.
constexpr uint8_t CC_TRACK_VOL  = 132;  // scheduleTrackVolume  (authored byte /255)
constexpr uint8_t CC_MASTER_VOL = 133;  // scheduleMasterVolume (authored byte /255), TRACK_GLOBAL

// The filter switched ON (LPF / HPF / BPF): the id carries the TYPE, the value the cutoff, so both
// land on the same frame — split, the filter would open a block before changing shape (a click).
constexpr uint8_t CC_FILTER_LP  = 134;  // scheduleVoiceFilterMode(type 1) (authored byte /255)
constexpr uint8_t CC_FILTER_HP  = 135;  // scheduleVoiceFilterMode(type 2)
constexpr uint8_t CC_FILTER_BP  = 136;  // scheduleVoiceFilterMode(type 3)

/** The FilterModule type a CC_FILTER_LP/HP/BP id turns on (1 lp | 2 hp | 3 bp), or 0 for any other. */
constexpr int cc_filter_mode(int param) {
    return param == CC_FILTER_LP ? 1 : param == CC_FILTER_HP ? 2 : param == CC_FILTER_BP ? 3 : 0;
}

// The dirt commands.
// ⚠️ 137 is retired with the command that used it and stays unused.
// ⚠️ CC_CRUSH's value is two 4-bit numbers, not a quantity: never interpolate it.
constexpr uint8_t CC_DRIVE      = 138;  // scheduleVoiceDrive     (authored byte /255)
constexpr uint8_t CC_CRUSH      = 139;  // scheduleVoiceCrush     (authored byte /255, two nibbles)

// Fine tune (FIN). MIDI's fine tune is RPN 1 and retunes the whole channel, so nothing goes on the wire.
constexpr uint8_t CC_FINE_TUNE  = 140;  // scheduleVoiceFineTune  (authored byte /255)

// The loop-window slide (LPO).
// ⚠️ The value is a STEP, not a position: two LPO records slide twice, so a lost record loses a
// movement, not just a value.
constexpr uint8_t CC_LOOP_SLIDE = 141;  // scheduleVoiceLoopSlide (authored byte /255)

// The delay's echo time (TIM). ⚠️ Rides TRACK_GLOBAL like CC_MASTER_VOL: the delay is a shared send.
constexpr uint8_t CC_DELAY_TIME = 142;  // scheduleDelayTime (authored byte /255), TRACK_GLOBAL

/** Slot index 0-3 for CC_SLOT_A..D, or -1 for a literal controller number. */
constexpr int cc_slot_index(uint8_t param) {
    return (param >= CC_SLOT_A && param <= CC_SLOT_D) ? param - CC_SLOT_A : -1;
}

// The full NoteOn trigger bundle, tapped at the engine's scheduleNote entry. NoteOns for an invalid
// instrument or an empty slot ARE events; consumers drop them.
//  * note = the MIDI number, (octave+1)*12 + pitch, so octave −1 from transpose stays non-negative.
//    Authored top-octave notes can exceed 127 (B-9 = 131); consumers clamp, traces record verbatim.
//  * velocity −1 = derive from velGain (retrig/arp).
//  * ⚠️ velGain rides the engine arg NAMED `volume` and volGain rides `phraseVol` — the names are
//    crossed (note-queue.h); copy the wiring, not the names.
//  * start = the authored OFF byte. The slice window and table tic rate are derived below the seam.
//  * transpose (chain+song), pit and arp are separate fields: slice selection needs the chain
//    transpose alone. MIDI out folds all three into data1.
//  * pslDur is in TICKS, pbnRate the raw FX byte /16, vibSpd Hz already tempo-scaled, vibDep semitones.
struct NoteOnPayload {
    uint8_t  note;        // note      MIDI number (octave+1)*12+pitch, 0-131 practical
    int8_t   velocity;    // vel       -1 | 0-127 (phrase V column)
    uint32_t velGainBits; // velGain   f32ᵇ (velocity curve, squared) — seam arg `volume`
    uint32_t volGainBits; // volGain   f32ᵇ (instr vol | Vxx/255) — seam arg `phraseVol`
    uint32_t panBits;     // pan       f32ᵇ 0=L ½=C 1=R (PAN-with-note baked here)
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

/** The note's velocity, 0..1 — the V column, or for velocity −1 the root of the squared velGain.
 *  ⚠️ Every consumer that turns a note into a velocity reads it here: a retrigger carries only
 *  velGain, and a consumer that read it differently would hear the note and its repeats apart. */
inline float note_velocity_unit(const NoteOnPayload& n) {
    if (n.velocity >= 0) return static_cast<float>(n.velocity) / 127.0f;
    float g;
    std::memcpy(&g, &n.velGainBits, sizeof g);
    return g > 0.0f ? std::sqrt(g) : 0.0f;
}

struct NoteOffPayload  { uint8_t mode; };                          // mode      NOTE_OFF_*
struct CcPayload       { uint8_t param; uint32_t valueBits; };     // param value
struct ProgramPayload  { uint8_t program; };                       // program   0-127
// value14: the MPB byte `<< 6`, so 0x80 lands exactly on centre 0x2000. 0xFF reaches 16320 of 16383;
// an exact centre matters more, since a bend that does not rest at zero detunes every note.
struct PitchBendPayload{ uint16_t value14; };                      // value     centre 0x2000
struct ExtPitchRatePayload { uint32_t rateBits; uint16_t tempo; }; // rate tempo (rate: raw byte /16)
struct ExtVibratoPayload   { uint32_t speedBits, depthBits; };     // speed depth
struct ExtTableRowPayload  { uint8_t row; };                       // row       0-15
struct ExtReversePayload   { uint8_t reverse, restart; };          // reverse restart
struct ExtEqSlotPayload    { int16_t slot; };                      // slot      -1 bypass | 0-127
struct ExtMasterEqPayload  { int16_t slot; };                      // slot      -1 bypass | 0-127

// One EQ setting as three bands of AUTHORED hex, not Hz/dB/Q: interpolating hex is linear in
// log-frequency, and it keeps morph values in the integer domain the goldens hold.
// ⚠️ `type` is not interpolated — the start preset's type holds for the whole span (automation_curve.h).
struct ExtEqMorphPayload {                                         // type0..2 freq0..2 gain0..2 q0..2
    uint8_t type[3];   // 0 OFF | 1 LOSHELF | 2 LOWCUT | 3 BELL | 4 HISHELF | 5 HICUT (eq-module.h)
    uint8_t freq[3];   // 00-FF → 20-20000 Hz, log
    uint8_t gain[3];   // 0-240 → −12.0..+12.0 dB, 0.1 dB a step
    uint8_t q[3];      // 00-FF → 0.1-10.0, log
};

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
        ExtEqMorphPayload   extEqMorph;      // EV_EXT_EQ_MORPH and EV_EXT_MASTER_EQ_MORPH both
    };
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_EVENT_H
