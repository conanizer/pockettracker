#ifndef POCKETTRACKER_SONGCORE_ROUTER_H
#define POCKETTRACKER_SONGCORE_ROUTER_H

// ─── The MIDI event bus / router ─────────────────────────────────────────────────────────────────
//
// The scheduler talks to the engine through this: one method per kind of engine call, each building
// one songcore::Event (event.h) and handing it to every attached IMidiConsumer. The trace text
// (trace_writer.h) is just one consumer.
//
// Nothing here derives audio: seam arguments are copied verbatim (floats as raw binary32 bits). The
// SF velocity curve, slice window and baseFreq stay consumer-side, never on the bus.
// Transport (t_play / t_stop) is not an Event; it passes straight to on_play / on_stop.

#include <cstdint>
#include <cstring>
#include <string>
#include "event.h"

namespace songcore {

// ─── NoteOn seam arguments ────────────────────────────────────────────────────────────────────────
// The engine's scheduleNote arguments, with defaults, so call sites read like named-argument calls.
// The router folds pitch/octave into the MIDI number, (octave+1)*12+pitch.
struct NoteArgs {
    int64_t frame        = 0;
    int     track        = 0;
    int     instrument   = 0;
    int     notePitch    = 0;   // note.pitch  (0-11)
    int     noteOctave   = 0;   // note.octave
    int     velocity     = -1;  // midiVelocity: -1 legacy derive | 0-127
    float   velGain      = 1.0f;  // engine arg `volume`    (velocity curve)
    float   volGain      = 1.0f;  // engine arg `phraseVol` (instr vol | Vxx/255)
    float   pan          = 0.5f;
    int     start        = -1;  // startPointOverride
    int     slice        = -1;  // sliIndex
    int     transpose    = 0;   // transposeSemitones
    int     pit          = 0;   // pitSemitones
    int     arp          = 0;   // arpSemitoneOffset
    int     tableId      = -1;  // tableIdOverride
    int     tableRow     = -1;  // tableStartRow
    float   pslOff       = 0.0f;
    float   pslDur       = 0.0f;
    float   pbnRate      = 0.0f;
    float   vibSpd       = 0.0f;
    float   vibDep       = 0.0f;
};

// ─── Consumer interface ─────────────────────────────────────────────────────────────────────────
// on_play / on_stop carry the session context the trace needs; audio consumers may ignore them.
struct IMidiConsumer {
    virtual ~IMidiConsumer() = default;
    virtual void consume(const Event& ev) = 0;
    virtual void on_play(const std::string& kind, const std::string& detail,
                         int64_t start_frame, int tempo, int sample_rate) = 0;
    virtual void on_stop() = 0;
};

// ─── Which instrument is a track's events for? ───────────────────────────────────────────────────
//
// ⚠️ Only NoteOn carries an instrument; NoteOff, CC and EXT events are TRACK-SCOPED (`INSTRUMENT_NONE`).
// With two consumers routing per instrument, "is this note-off mine?" needs an answer, so it is
// derived once here: the last NoteOn on a track names the instrument of what follows. Both consumers
// keep one, so they reach the same verdict — or a note plays twice or by nobody.
// ⚠️ Assumes per-track events arrive in non-decreasing frame order, which the scheduler guarantees.
struct TrackInstruments {
    // 0-7 sequencer + TRACK_PREVIEW (8). TRACK_GLOBAL never resolves to an instrument.
    static constexpr int LANES = TRACK_PREVIEW + 1;

    void reset() { for (int i = 0; i < LANES; ++i) lane_[i] = INSTRUMENT_NONE; }

    /** The instrument the track's events are currently for, WITHOUT consuming an event. */
    int16_t current(uint8_t track) const {
        return track < LANES ? lane_[track] : INSTRUMENT_NONE;
    }

    /** Learn from `ev` (a NoteOn re-points the track) and return the instrument it belongs to. */
    int16_t observe(const Event& ev) {
        if (ev.track >= LANES) return INSTRUMENT_NONE;          // TRACK_GLOBAL — EQM and friends
        if (ev.type == EV_NOTE_ON) lane_[ev.track] = ev.instrument;
        return lane_[ev.track];
    }

  private:
    int16_t lane_[LANES] = {INSTRUMENT_NONE, INSTRUMENT_NONE, INSTRUMENT_NONE, INSTRUMENT_NONE,
                            INSTRUMENT_NONE, INSTRUMENT_NONE, INSTRUMENT_NONE, INSTRUMENT_NONE,
                            INSTRUMENT_NONE};
};

// ─── The router ─────────────────────────────────────────────────────────────────────────────────
// Fans each record out to every consumer — the engine consumer, the cable, and the trace writer when
// tracing — in the same order, so a trace is a faithful witness of what was scheduled. Consumers are
// owned by the host; attach/detach happens on the transport thread, never mid-dispatch.
struct MidiRouter {
    static constexpr int MAX_CONSUMERS = 4;

    explicit MidiRouter(IMidiConsumer* c = nullptr) { add_consumer(c); }

    void add_consumer(IMidiConsumer* c) {
        if (!c || count_ >= MAX_CONSUMERS) return;
        for (int i = 0; i < count_; ++i) if (consumers_[i] == c) return;   // idempotent
        consumers_[count_++] = c;
    }
    void remove_consumer(IMidiConsumer* c) {
        for (int i = 0; i < count_; ++i) {
            if (consumers_[i] != c) continue;
            for (int j = i; j + 1 < count_; ++j) consumers_[j] = consumers_[j + 1];
            --count_;
            return;
        }
    }
    void clear_consumers() { count_ = 0; }

    // ── transport ──
    void t_play(const std::string& kind, const std::string& detail,
                int64_t start_frame, int tempo, int sample_rate) {
        for (int i = 0; i < count_; ++i) consumers_[i]->on_play(kind, detail, start_frame, tempo, sample_rate);
    }
    void t_stop() { for (int i = 0; i < count_; ++i) consumers_[i]->on_stop(); }

    // ── events (build the record, forward to the consumer) ──

    void note_on(const NoteArgs& a) {
        Event ev = base(a.frame, a.track, static_cast<int16_t>(a.instrument), EV_NOTE_ON);
        NoteOnPayload& n = ev.noteOn;
        n.note        = static_cast<uint8_t>((a.noteOctave + 1) * 12 + a.notePitch);
        n.velocity    = static_cast<int8_t>(a.velocity);
        n.velGainBits = bits(a.velGain);
        n.volGainBits = bits(a.volGain);
        n.panBits     = bits(a.pan);
        n.start       = a.start;
        n.slice       = a.slice;
        n.transpose   = a.transpose;
        n.pit         = a.pit;
        n.arp         = a.arp;
        n.tableId     = a.tableId;
        n.tableRow    = a.tableRow;
        n.pslOffBits  = bits(a.pslOff);
        n.pslDurBits  = bits(a.pslDur);
        n.pbnRateBits = bits(a.pbnRate);
        n.vibSpdBits  = bits(a.vibSpd);
        n.vibDepBits  = bits(a.vibDep);
        emit(ev);
    }

    void note_off(int64_t frame, int track, int mode) {
        Event ev = base(frame, track, INSTRUMENT_NONE, EV_NOTE_OFF);
        ev.noteOff.mode = static_cast<uint8_t>(mode);
        emit(ev);
    }

    void cc(int64_t frame, int track, int param, float value) {
        Event ev = base(frame, track, INSTRUMENT_NONE, EV_CC);
        ev.cc.param = static_cast<uint8_t>(param);
        ev.cc.valueBits = bits(value);
        emit(ev);
    }

    // ── MPG / MPB ──
    // Track-scoped like CC: they act on whatever the track is playing (TrackInstruments).

    void program(int64_t frame, int track, int program) {
        Event ev = base(frame, track, INSTRUMENT_NONE, EV_PROGRAM);
        ev.program.program = static_cast<uint8_t>(program & 0x7F);
        emit(ev);
    }

    /** `value14` is the full 14-bit value, centre 0x2000 — the caller does the byte→14-bit widening. */
    void pitch_bend(int64_t frame, int track, int value14) {
        Event ev = base(frame, track, INSTRUMENT_NONE, EV_PITCH_BEND);
        ev.pitchBend.value14 = static_cast<uint16_t>(value14 & 0x3FFF);
        emit(ev);
    }

    void ext_pitch_rate(int64_t frame, int track, float rate, int tempo) {
        Event ev = base(frame, track, INSTRUMENT_NONE, EV_EXT_PITCH_RATE);
        ev.extPitchRate.rateBits = bits(rate);
        ev.extPitchRate.tempo = static_cast<uint16_t>(tempo);
        emit(ev);
    }

    void ext_vibrato(int64_t frame, int track, float speed, float depth) {
        Event ev = base(frame, track, INSTRUMENT_NONE, EV_EXT_VIBRATO);
        ev.extVibrato.speedBits = bits(speed);
        ev.extVibrato.depthBits = bits(depth);
        emit(ev);
    }

    void ext_table_row(int64_t frame, int track, int row) {
        Event ev = base(frame, track, INSTRUMENT_NONE, EV_EXT_TABLE_ROW);
        ev.extTableRow.row = static_cast<uint8_t>(row);
        emit(ev);
    }

    void ext_reverse(int64_t frame, int track, bool reverse, bool restart) {
        Event ev = base(frame, track, INSTRUMENT_NONE, EV_EXT_REVERSE);
        ev.extReverse.reverse = reverse ? 1 : 0;
        ev.extReverse.restart = restart ? 1 : 0;
        emit(ev);
    }

    void ext_eq_slot(int64_t frame, int track, int slot) {
        Event ev = base(frame, track, INSTRUMENT_NONE, EV_EXT_EQ_SLOT);
        ev.extEqSlot.slot = static_cast<int16_t>(slot);
        emit(ev);
    }

    // EQM is global: TRACK_GLOBAL (255), no instrument.
    void ext_master_eq(int64_t frame, int slot) {
        Event ev = base(frame, TRACK_GLOBAL, INSTRUMENT_NONE, EV_EXT_MASTER_EQ);
        ev.extMasterEq.slot = static_cast<int16_t>(slot);
        emit(ev);
    }

    // An AUS/AUF tick over EQN/EQM: band VALUES, not a slot — the setting it names need not be one
    // any preset holds (automation.h, eq_morph_at).
    void ext_eq_morph(int64_t frame, int track, const ExtEqMorphPayload& bands) {
        Event ev = base(frame, track, INSTRUMENT_NONE, EV_EXT_EQ_MORPH);
        ev.extEqMorph = bands;
        emit(ev);
    }

    void ext_master_eq_morph(int64_t frame, const ExtEqMorphPayload& bands) {
        Event ev = base(frame, TRACK_GLOBAL, INSTRUMENT_NONE, EV_EXT_MASTER_EQ_MORPH);
        ev.extEqMorph = bands;
        emit(ev);
    }

  private:
    static uint32_t bits(float v) {
        uint32_t b;
        std::memcpy(&b, &v, sizeof b);
        return b;
    }
    static Event base(int64_t frame, int track, int16_t instrument, uint8_t type) {
        Event ev{};
        ev.frame = frame;
        ev.track = static_cast<uint8_t>(track);
        ev.instrument = instrument;
        ev.type = type;
        return ev;
    }
    void emit(const Event& ev) { for (int i = 0; i < count_; ++i) consumers_[i]->consume(ev); }

    IMidiConsumer* consumers_[MAX_CONSUMERS] = {nullptr, nullptr, nullptr, nullptr};
    int count_ = 0;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_ROUTER_H
