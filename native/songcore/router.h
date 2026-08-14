#ifndef POCKETTRACKER_SONGCORE_ROUTER_H
#define POCKETTRACKER_SONGCORE_ROUTER_H

// ─── The MIDI event bus / router ─────────────────────────────────────────────────────────────────
//
// The songcore spine (Phase 1 S4). The scheduler (scheduler.h) speaks to a MidiRouter through the
// SAME seam the Kotlin sequencer used against AudioEngine — one method per AudioEngine.schedule*
// call that carried an EventTrace tap. Each method builds one songcore::Event (event.h, encoding 1)
// and hands it to the attached IMidiConsumer. This is where the Kotlin "tap wrapper around each
// schedule* call" (event-schema §6) becomes "the router itself" — bus records are the primary form,
// the trace text (trace_writer.h) is one consumer of them.
//
// Nothing here derives audio: the router copies the seam arguments verbatim into the record (the
// float fields as raw binary32 bits), exactly as AudioEngine.kt's tap did at ENTRY — every value
// below the seam (SF velocity curve, slice window, baseFreq) stays consumer-side and never rides an
// event. That is what makes the C++ event stream reproduce the Kotlin golden byte-for-byte.
//
// Transport (t_play / t_stop) is not a bus Event — it frames a PLAY..STOP session for the trace
// consumer, so it passes straight through to IMidiConsumer::on_play / on_stop.

#include <cstdint>
#include <cstring>
#include <string>
#include "event.h"

namespace songcore {

// ─── NoteOn seam arguments ────────────────────────────────────────────────────────────────────────
// Mirrors the AudioEngine.scheduleNote signature (the subset the tap records) field-for-field,
// including the Kotlin defaults, so scheduler call sites read like the originals' named-argument
// calls. `note` is passed as its pitch/octave pair (as the Note object was at the seam); the router
// folds it to the MIDI number for the record, matching EventTrace.noteOn's (octave+1)*12+pitch.
struct NoteArgs {
    int64_t frame        = 0;
    int     track        = 0;
    int     instrument   = 0;
    int     notePitch    = 0;   // note.pitch  (0-11)
    int     noteOctave   = 0;   // note.octave
    int     velocity     = -1;  // midiVelocity: -1 legacy derive | 0-127
    float   velGain      = 1.0f;  // seam arg `volume`   (velocity curve)
    float   volGain      = 1.0f;  // seam arg `phraseVol` (instr vol | Vxx/255)
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
// A bus consumer sees the same records the future EXTERNAL/sampler/SF consumers will (MIDI plan
// §3). For S4 the only consumer is the trace writer; on_play/on_stop carry the session context the
// trace needs and every other (audio) consumer ignores.
struct IMidiConsumer {
    virtual ~IMidiConsumer() = default;
    virtual void consume(const Event& ev) = 0;
    virtual void on_play(const std::string& kind, const std::string& detail,
                         int64_t start_frame, int tempo, int sample_rate) = 0;
    virtual void on_stop() = 0;
};

// ─── Which instrument is a track's events for? ───────────────────────────────────────────────────
//
// ⚠️ ONLY NoteOn CARRIES AN INSTRUMENT. NoteOff, CC and every EXT event are TRACK-SCOPED and ride
// `INSTRUMENT_NONE` (event.h) — they act on whatever is sounding on the track. That is fine for a
// consumer which owns every track, and it is exactly what breaks the moment there are TWO consumers
// and the routing decision is per-instrument: "is this note-off mine?" has no answer in the record.
//
// So the answer is derived, once, in one place: the last NoteOn on a track names the instrument every
// following track-scoped event belongs to. Both consumers keep one of these and both therefore reach
// the SAME verdict for the same event — which is the property that matters, because a disagreement
// means a note is either played twice or by nobody.
//
// ⚠️ It assumes events arrive per track in non-decreasing frame order, which the scheduler guarantees
// (it schedules forward, and retrig/arp notes are emitted inside the step that spawns them).
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
// Fans one record out to every attached consumer (MIDI plan §3: "sources → router → consumers"). In
// the app that is the engine consumer plus, when tracing, the trace writer — they see the identical
// record in the identical order, which is what makes a device trace a faithful witness of the audio
// that was actually scheduled. Consumers are plain pointers owned by the host; attach/detach happens
// on the transport thread, never during a dispatch.
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

    // ── MIDI phase D (MPG / MPB) ──
    //
    // Track-scoped like CC, and for the same reason: the instrument they act on is whatever the track
    // is playing (TrackInstruments), not a routing key of their own.

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

    // EQM is global — the record rides TRACK_GLOBAL (255), instrument none (EventTrace.extMasterEq).
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
