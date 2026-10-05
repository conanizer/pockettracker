#ifndef POCKETTRACKER_SONGCORE_MIDI_OUT_H
#define POCKETTRACKER_SONGCORE_MIDI_OUT_H

// ─── The EXTERNAL consumer — bus events become MIDI bytes ────────────────────────────────────────
//
// The mirror of engine_consumer.h: the same bus record, turned into bytes on a cable. Platform-free;
// `IMidiOut` is the seam (ALSA rawmidi, winmm, Android MidiManager behind it).
//
// Kept in ONE place on purpose:
//   • the 0–255/0–1 → 0–127 scaling (`to7bit`), so internal and external behaviour cannot drift;
//   • the routing verdict (`instrument_routes_external` + `TrackInstruments`), shared with the engine
//     consumer;
//   • the note lifecycle: one active note per track, ended by a LEN gate, the next note, a KIL or the
//     transport stopping. External gear has no voice allocator — an unanswered note-on sounds until
//     the power is cut;
//   • the route to the wire: this serializer and `MidiClock` share `emit`, the OFFSET, the port and
//     the send observer.
//
// Events arrive at LOOKAHEAD time (ahead of the clock), are queued by target frame and released by
// `pump(now)` — from a ~1 kHz sender thread (`shell/midi-sender.h`) driven by `FrameEstimator`, or
// from `SongcoreHost::poll` when no such thread owns the job. Two threads, hence `mu_`.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "event.h"
#include "midi_clock.h"    // the 24 PPQN grid and the transport bytes
#include "model.h"
#include "note_tables.h"   // f32_from_bits
#include "router.h"
#include "timing.h"

namespace songcore {

// ─── The platform seam ──────────────────────────────────────────────────────────────────────────

/**
 * A MIDI output port, implemented once per platform. `send` gets a complete message (2 or 3 bytes)
 * and must not block for long — it runs on whichever thread pumps the queue.
 */
struct IMidiOut {
    virtual ~IMidiOut() = default;

    /** How many output devices exist right now. Re-enumerated on demand; hot-plug changes it. */
    virtual int device_count() = 0;
    /** Display name of device `index`, for the MIDI screen's OUTPUT row. */
    virtual std::string device_name(int index) = 0;

    virtual bool open(int index) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    virtual void send(const uint8_t* data, int len) = 0;

    /** True once the open port has failed in a way only a reopen can cure (the cable was pulled). */
    virtual bool broken() const { return false; }

    /** A synth built into the OS rather than a device someone plugged in — AUTO passes over it. */
    virtual bool is_builtin_synth(int /*index*/) { return false; }
};

// ─── Scaling — the ONE place a wide internal value becomes a 7-bit one ───────────────────────────

/** A 0..1 gain (pan, sends, volume — every float on the bus) to a 0–127 controller value. */
inline int to7bit(float v01) {
    if (!(v01 > 0.0f)) return 0;      // also catches NaN
    if (v01 >= 1.0f) return 127;
    return static_cast<int>(v01 * 127.0f + 0.5f);
}

/** An authored 0x00–0xFF byte to a 0–127 controller value. */
inline int byte_to_7bit(int b) {
    if (b <= 0) return 0;
    if (b >= 255) return 127;
    return (b * 127 + 127) / 255;
}

/** MIDI status bytes. */
constexpr uint8_t MIDI_NOTE_OFF = 0x80, MIDI_NOTE_ON = 0x90, MIDI_CC = 0xB0,
                  MIDI_PROGRAM  = 0xC0, MIDI_PITCH_BEND = 0xE0;

/** Controller numbers this file sends by name. */
constexpr uint8_t MIDI_CC_BANK_MSB = 0, MIDI_CC_BANK_LSB = 32, MIDI_CC_ALL_NOTES_OFF = 123;

/** No bend — the 14-bit value a channel rests at, and what a panic restores it to. */
constexpr int MIDI_BEND_CENTRE = 0x2000;

/**
 * One message, stamped with the frame it is due on.
 * `seq` makes the order TOTAL: messages due on the same frame leave in production order (bank →
 * program → CC → note-on), which a sort on frame alone could swap.
 * `frame == UNSCHEDULED` marks a panic message sent past the clock (`send_now`); a lateness observer
 * must skip it.
 */
struct MidiMessage {
    static constexpr int64_t UNSCHEDULED = INT64_MIN;

    int64_t  frame = 0;
    uint32_t seq   = 0;
    uint8_t  bytes[3] = {0, 0, 0};
    uint8_t  len   = 0;
};

/**
 * Told about every message as it is released — the only way to measure how close to its due frame
 * a message leaves (`shell/midi-sender.h` fits its own wall clock against `dueFrame`).
 * ⚠️ Called under the lock, from whichever thread released the message: be quick, and never call back
 * into `ExternalConsumer`.
 * Called even with no port open (`sent` says so), so timing can be measured with no hardware.
 */
struct IMidiSendObserver {
    virtual ~IMidiSendObserver() = default;
    virtual void on_released(const MidiMessage& m, int64_t nowFrame, bool sent) = 0;
};

// ─── The consumer ───────────────────────────────────────────────────────────────────────────────

class ExternalConsumer : public IMidiConsumer {
  public:
    explicit ExternalConsumer(const Project* project) : project_(project) { forget_channel_state(); }
    ~ExternalConsumer() override { panic(); }

    // ── ⚠️ THREADING ─────────────────────────────────────────────────────────────────────────────
    //
    //   • the FRAME LOOP produces (`consume`, `on_play`, previews) and PANICS (stop, port swap,
    //     teardown);
    //   • the SENDER THREAD only calls `pump`.
    //
    // Every public method takes `mu_`; every private helper assumes it is held (hence `panic_locked()`
    // inside `on_play` / `set_out`). `std::mutex` is not recursive.
    // ⚠️ The port write happens under the lock on purpose: `IMidiOut::send` need not be thread-safe
    // (ALSA rawmidi's is not). A blocking write stalls the producer, bounded by the 4 KB rawmidi buffer
    // — far better than a dropped note-off.

    /**
     * Attach (or detach, with nullptr) the port.
     * ⚠️ Panics FIRST: after a swap, the owed note-offs can no longer reach the device holding the notes.
     */
    void set_out(IMidiOut* out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (out == out_) return;
        panic_locked();
        out_ = out;
    }
    IMidiOut* out() const {
        std::lock_guard<std::mutex> lk(mu_);
        return out_;
    }

    /**
     * The bus's track→instrument state, read-only, for MIDI in's router — never a third opinion.
     * ⚠️ Not locked: `tracks_` is written in `consume` and read here, both on the frame-loop thread; the
     * sender thread never touches it. A second writer would invalidate this.
     */
    const TrackInstruments& track_instruments() const { return tracks_; }

    /** The send-timing observer, or nullptr. Set once at boot, before the sender thread starts. */
    void set_send_observer(IMidiSendObserver* obs) {
        std::lock_guard<std::mutex> lk(mu_);
        observer_ = obs;
    }

    /**
     * SYNC OUT — the 24 PPQN clock and the transport bytes (midi_clock.h).
     * ⚠️ Turning it OFF mid-take sends the owed Stop; a synth left on external sync would otherwise wait
     * forever, looking like a crash.
     * Turning it ON mid-take takes effect at the next start: the grid's epoch is the take's start frame,
     * and a mid-take clock would have to invent one.
     */
    void set_sync_out(bool on) {
        std::lock_guard<std::mutex> lk(mu_);
        if (clock_.set_enabled(on)) send_now_1(MIDI_RT_STOP);
    }
    bool sync_out() const {
        std::lock_guard<std::mutex> lk(mu_);
        return clock_.enabled();
    }

    /** Alignment between speaker audio and the cable, in ms (positive = MIDI later), applied as frames
     *  at send time. */
    void set_offset_ms(int ms) {
        std::lock_guard<std::mutex> lk(mu_);
        offsetMs_ = ms;
    }
    int offset_ms() const {
        std::lock_guard<std::mutex> lk(mu_);
        return offsetMs_;
    }

    // ── IMidiConsumer ────────────────────────────────────────────────────────────────────────────

    /**
     * `on_play` IS the transport starting (unlike `on_stop`): `t_play` comes once per take. The render
     * pass never reaches this consumer — the host detaches the cable.
     * ⚠️ The `panic()` is what makes `tracks_.reset()` safe: reset with notes still active, their lanes
     * no longer resolve as external and the owed note-offs are never sent — fatal on the PREVIEW lane,
     * which has no next note-on.
     */
    void on_play(const std::string& kind, const std::string& detail, int64_t startFrame,
                 int tempo, int sample_rate) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (tempo > 0) tempo_ = tempo;
        if (sample_rate > 0) sampleRate_ = sample_rate;
        // A fresh take is a fresh device state: what each channel was last told may no longer be
        // true. (`panic_locked` ends notes, drops the queue, sends an owed Stop and forgets channel
        // state.)
        panic_locked();
        tracks_.reset();

        // ── arm the clock, and say WHERE the take starts ─────────────────────────────────────────
        //
        // The start row is read from `detail`, data already flowing on exactly this event — rather
        // than three play verbs each remembering to set it. Only SONG carries a row (`row=`); CHAIN
        // and PHRASE start the device at 0.
        const int startRow = kind == "SONG" ? hex_after(detail, "row=") : 0;
        const int spp      = (startRow > 0 && project_) ? nominal_spp_beats(*project_, startRow) : 0;
        clock_.start(startFrame, frames_per_quarter(), spp);
    }

    /**
     * ⚠️ Deliberately empty — `t_stop` is NOT "the transport stopped". It closes a PLAY..STOP trace
     * segment and ends EVERY scheduling pass, including a whole-song pass; panicking here would discard
     * the take the moment it was queued. The real stop is `SongcoreHost::stop()` → `panic()`.
     */
    void on_stop() override {}

    void consume(const Event& ev) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!project_) return;

        // Learn the track→instrument map from EVERY note-on, internal ones included, or a track that
        // goes SAMPLER → EXTERNAL would resolve to the old instrument.
        const int16_t prev  = tracks_.current(ev.track);
        const int16_t instr = tracks_.observe(ev);

        if (!routes_external(instr)) {
            // ⚠️ A track leaving EXTERNAL owes the device a note-off; no later event on it will reach
            // this consumer to deliver one. (The mirror of EngineConsumer's flip kill.)
            if (ev.type == EV_NOTE_ON && routes_external(prev)) end_note(ev.track, ev.frame);
            return;
        }
        const Instrument& ins = project_->instruments[static_cast<size_t>(instr)];

        switch (ev.type) {
            case EV_NOTE_ON:   note_on(ev, ins);  break;
            case EV_NOTE_OFF:  end_note(ev.track, ev.frame); break;   // KIL and ADSR release alike
            case EV_CC:        cc_event(ev, ins); break;
            case EV_PROGRAM: program_event(ev, ins); break;
            case EV_PITCH_BEND: pitch_bend_event(ev, ins); break;
            default:
                // EV_EXT_* have no MIDI 1.0 form; an EXT event is internal by definition.
                break;
        }
    }

    // ── The clock side ───────────────────────────────────────────────────────────────────────────

    /**
     * Release everything due at or before `nowFrame` and close expired LEN gates. Late, never early,
     * so the caller's cadence is the precision (shell/midi-sender.h).
     */
    void pump(int64_t nowFrame) {
        std::lock_guard<std::mutex> lk(mu_);
        // ONE timeline: `due` is "now" in EVENT frames, so LEN gates and the queue share the OFFSET.
        const int64_t due = nowFrame - offset_frames();
        // ── The clock first ──────────────────────────────────────────────────────────────────────
        // Transport bytes ride tick 0's frame and Start must precede the take. Real-time bytes may be
        // interleaved anywhere, so clock-before-notes costs nothing. It uses the OFFSET-corrected
        // `due` too, or OFFSET would mean two things on one cable.
        clock_.pump(due, frames_per_quarter(), [&](int64_t at, const uint8_t* bytes, int len) {
            emit_bytes(at, bytes, len, nowFrame);
        });
        // Expired gates first, so their note-offs sort into the queue ahead of anything later.
        for (int t = 0; t < TrackInstruments::LANES; ++t) {
            if (active_[t].on && active_[t].offFrame <= due)
                end_note(static_cast<uint8_t>(t), active_[t].offFrame);
        }
        size_t i = 0;
        while (i < pending_.size() && pending_[i].frame <= due) {
            emit(pending_[i], nowFrame);
            ++i;
        }
        if (i > 0) pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    /**
     * All notes off, now, on every channel touched — and the pending queue dropped.
     * Per-note offs end what we know about; CC 123 is the backstop for what our bookkeeping lost. The
     * queue holds note-ons for a take that no longer exists.
     */
    void panic() {
        std::lock_guard<std::mutex> lk(mu_);
        panic_locked();
    }

    /** Pending messages not yet released — the tools' window into the queue. */
    size_t pending_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_.size();
    }

    /**
     * Does this consumer owe anything SOON? The sender thread's busy/idle test, derived from the
     * deadlines: a running clock owes a tick, a non-empty queue a message.
     * ⚠️ The clock queues nothing, so `pending_count() > 0` alone would idle the thread during pure
     * sync out — 20% of a tick in jitter. LEN gates are deliberately excluded (4 ms idle bound is
     * inaudible; shell/midi-sender.cpp).
     */
    bool needs_fast_pump() const {
        std::lock_guard<std::mutex> lk(mu_);
        return !pending_.empty() || clock_.running();
    }

    /** The clock's counters, for tools and diagnostics. Never for a decision. */
    int64_t clock_tick_index() const {
        std::lock_guard<std::mutex> lk(mu_);
        return clock_.tick_index();
    }
    int clock_dropped_ticks() const {
        std::lock_guard<std::mutex> lk(mu_);
        return clock_.dropped_ticks();
    }

  private:
    void panic_locked() {
        // ⚠️ The Stop first: it stops the device's own sequencer at the same instant we end our notes.
        if (clock_.stop()) send_now_1(MIDI_RT_STOP);
        for (int t = 0; t < TrackInstruments::LANES; ++t) {
            if (!active_[t].on) continue;
            send_now(MIDI_NOTE_OFF | active_[t].channel, active_[t].note, 0);
            active_[t].on = false;
        }
        for (int c = 0; c < 16; ++c) {
            if (!channelUsed_[c]) continue;
            send_now(MIDI_CC | static_cast<uint8_t>(c), MIDI_CC_ALL_NOTES_OFF, 0);
        }
        // A bent channel is silent and still wrong (see pitch_bend_event). Independent of
        // channelUsed_: an MPB with no note still bent the device.
        for (int c = 0; c < 16; ++c) {
            if (!bent_[c]) continue;
            send_now(MIDI_PITCH_BEND | static_cast<uint8_t>(c),
                     MIDI_BEND_CENTRE & 0x7F, (MIDI_BEND_CENTRE >> 7) & 0x7F);
        }
        pending_.clear();
        forget_channel_state();
    }

    struct ActiveNote {
        bool    on       = false;
        uint8_t channel  = 0;
        uint8_t note     = 0;
        int64_t offFrame = INT64_MAX;   // INT64_MAX = gate-to-next (midiLen 0): no timed off
    };

    /** The routing verdict, asked of the model (model.h) so both consumers cannot disagree. */
    bool routes_external(int16_t instrument) const {
        if (instrument < 0 || !project_) return false;
        if (static_cast<size_t>(instrument) >= project_->instruments.size()) return false;
        return instrument_routes_external(project_->instruments[static_cast<size_t>(instrument)]);
    }

    // ── note lifecycle ───────────────────────────────────────────────────────────────────────────

    void note_on(const Event& ev, const Instrument& ins) {
        const uint8_t ch = chan(ins);
        // The track's previous note ends where this one begins — the only end when midiLen is 0.
        end_note(ev.track, ev.frame);

        if (project_->midiSendProgramChange) send_program(ev.frame, ch, ins);
        send_cc_defaults(ev.frame, ch, ins);

        // PAN rides CC 10, sent only when it changes.
        const int pan7 = to7bit(f32_from_bits(ev.noteOn.panBits));
        if (lastPan_[ch] != pan7) {
            send_at(ev.frame, MIDI_CC | ch, CC_PAN, static_cast<uint8_t>(pan7));
            lastPan_[ch] = pan7;
        }

        const int note = midi_note(ev.noteOn);
        const int vel  = midi_velocity(ev.noteOn);
        send_at(ev.frame, MIDI_NOTE_ON | ch, static_cast<uint8_t>(note), static_cast<uint8_t>(vel));

        ActiveNote& a = active_[ev.track];
        a.on = true;
        a.channel = ch;
        a.note = static_cast<uint8_t>(note);
        // ⚠️ Tempo from the live project, not `on_play`'s argument (the render path passes a fallback
        // there) — a LEN gate must not change length between play and render.
        const int tempo = project_->tempo > 0 ? project_->tempo : tempo_;
        a.offFrame = ins.midiLen > 0
                         ? ev.frame + ins.midiLen * frames_per_tic(frames_per_step(tempo, sampleRate_))
                         : INT64_MAX;
        channelUsed_[ch] = true;
    }

    /**
     * End the track's active note at `frame` or at its own LEN gate, whichever is FIRST.
     * ⚠️ The `min` is what makes LEN work: events arrive at lookahead time, so the NEXT note is usually
     * consumed before the gate is due, and without the `min` it would cancel the gate. A KIL before
     * the gate still wins — `min` cuts short, never extends.
     */
    void end_note(uint8_t track, int64_t frame) {
        if (track >= TrackInstruments::LANES) return;
        ActiveNote& a = active_[track];
        if (!a.on) return;
        send_at(std::min(frame, a.offFrame), MIDI_NOTE_OFF | a.channel, a.note, 0);
        a.on = false;
        a.offFrame = INT64_MAX;
    }

    // ── the note's two numbers ───────────────────────────────────────────────────────────────────

    /**
     * The three transpose fields, separate on the bus, folded into one note number here. Clamped:
     * B-9 = 131 is a legal phrase note but not legal MIDI.
     */
    static int midi_note(const NoteOnPayload& n) {
        const int v = static_cast<int>(n.note) + n.transpose + n.pit + n.arp;
        return v < 0 ? 0 : (v > 127 ? 127 : v);
    }

    /**
     * Velocity, derived in one place. `velocity` (the V column) wins when present; −1 means derive from
     * `velGain` = (V/127)², hence the square root. Then `volGain` (VOL or Vxx) scales it — the only
     * volume control an external instrument has.
     * Clamped to ≥ 1: velocity 0 IS a note-off.
     */
    static int midi_velocity(const NoteOnPayload& n) {
        const float unit = note_velocity_unit(n) * f32_from_bits(n.volGainBits);
        const int v = to7bit(unit);
        return v < 1 ? 1 : v;
    }

    // ── the patch bytes that ride a note-on ──────────────────────────────────────────────────────

    /**
     * Bank + program, when set and when the channel is not already on them. The `forget_channel_state()`
     * at each transport start makes this safe: a take's first note always states its patch.
     */
    void send_program(int64_t frame, uint8_t ch, const Instrument& ins) {
        if (ins.midiProgram < 0 && ins.midiBank < 0) return;
        if (lastBank_[ch] == ins.midiBank && lastProgram_[ch] == ins.midiProgram) return;
        if (ins.midiBank >= 0) {
            send_at(frame, MIDI_CC | ch, MIDI_CC_BANK_MSB, static_cast<uint8_t>((ins.midiBank >> 7) & 0x7F));
            send_at(frame, MIDI_CC | ch, MIDI_CC_BANK_LSB, static_cast<uint8_t>(ins.midiBank & 0x7F));
        }
        if (ins.midiProgram >= 0)
            send_at(frame, MIDI_PROGRAM | ch, static_cast<uint8_t>(ins.midiProgram & 0x7F));
        lastBank_[ch] = ins.midiBank;
        lastProgram_[ch] = ins.midiProgram;
    }

    /**
     * The instrument's CC slots, re-sent with EVERY note-on — not de-duplicated. They are the patch's
     * starting values; a `CCA` or a knob on the gear moves them between notes.
     */
    void send_cc_defaults(int64_t frame, uint8_t ch, const Instrument& ins) {
        for (const MidiCcSlot& s : ins.midiCC) {
            if (s.cc < 0 || s.value < 0) continue;
            send_at(frame, MIDI_CC | ch, static_cast<uint8_t>(s.cc & 0x7F),
                    static_cast<uint8_t>(s.value & 0x7F));
        }
    }

    /**
     * A bus CC onto the wire — a literal controller or a symbolic slot id.
     * ⚠️ A slot id (128-131) must never reach the `& 0x7F`: 128 masks to BANK SELECT. `resolve_cc_param`
     * (model.h) translates it, shared with the engine consumer.
     */
    void cc_event(const Event& ev, const Instrument& ins) {
        const int param = resolve_cc_param(ins, ev.cc.param);
        if (param < 0) return;    // an unassigned slot: nothing to move
        const uint8_t ch = chan(ins);
        const int v7 = to7bit(f32_from_bits(ev.cc.valueBits));
        send_at(ev.frame, MIDI_CC | ch, static_cast<uint8_t>(param & 0x7F), static_cast<uint8_t>(v7));
        if (param == CC_PAN) lastPan_[ch] = v7;   // keep the note-on de-dup honest
    }

    /**
     * MPG — an explicit program change on the track's channel.
     * ⚠️ It must update `lastProgram_`, or `send_program`'s de-dup would leave the instrument's next
     * note on the MPG's program. The bank becomes "unknown": a program change selects within whatever
     * bank the device is on.
     */
    void program_event(const Event& ev, const Instrument& ins) {
        const uint8_t ch = chan(ins);
        const int prog = ev.program.program & 0x7F;
        send_at(ev.frame, MIDI_PROGRAM | ch, static_cast<uint8_t>(prog));
        lastProgram_[ch] = prog;
        lastBank_[ch] = -2;
    }

    /**
     * MPB — an absolute 14-bit bend on the track's channel.
     * ⚠️ A bent channel is hanging state: every later note on that device plays detuned. Bent channels
     * are tracked and `panic_locked` centres them — only those, so we never touch another app's
     * channels.
     */
    void pitch_bend_event(const Event& ev, const Instrument& ins) {
        const uint8_t ch = chan(ins);
        const int v14 = ev.pitchBend.value14 & 0x3FFF;
        send_at(ev.frame, MIDI_PITCH_BEND | ch,
                static_cast<uint8_t>(v14 & 0x7F), static_cast<uint8_t>((v14 >> 7) & 0x7F));
        bent_[ch] = (v14 != MIDI_BEND_CENTRE);
    }

    // ── the queue ────────────────────────────────────────────────────────────────────────────────

    uint8_t chan(const Instrument& ins) const {
        const int c = ins.midiChannel;
        return static_cast<uint8_t>(c < 0 ? 0 : (c > 15 ? 15 : c));
    }

    int64_t offset_frames() const {
        return static_cast<int64_t>(offsetMs_) * sampleRate_ / 1000;
    }

    void send_at(int64_t frame, uint8_t status, uint8_t d1) { queue(frame, status, d1, 0, 2); }
    void send_at(int64_t frame, uint8_t status, uint8_t d1, uint8_t d2) { queue(frame, status, d1, d2, 3); }

    void queue(int64_t frame, uint8_t status, uint8_t d1, uint8_t d2, uint8_t len) {
        MidiMessage m;
        m.frame = frame;
        m.seq = seq_++;
        m.bytes[0] = status; m.bytes[1] = d1; m.bytes[2] = d2;
        m.len = len;
        // Sorted insert: the scheduler emits forward in time, so this is usually one comparison.
        // Ties keep arrival order (upper_bound on (frame, seq)).
        auto pos = std::upper_bound(pending_.begin(), pending_.end(), m,
                                    [](const MidiMessage& a, const MidiMessage& b) {
                                        return a.frame != b.frame ? a.frame < b.frame : a.seq < b.seq;
                                    });
        pending_.insert(pos, m);
    }

    /** Bypass the queue — panic only. Nothing else may skip the clock. */
    void send_now(uint8_t status, uint8_t d1, uint8_t d2) {
        MidiMessage m;
        m.frame = MidiMessage::UNSCHEDULED;   // never had a due time
        m.bytes[0] = status; m.bytes[1] = d1; m.bytes[2] = d2;
        m.len = 3;
        emit(m, MidiMessage::UNSCHEDULED);
    }

    /** The one-byte form — the real-time Stop only. */
    void send_now_1(uint8_t status) {
        MidiMessage m;
        m.frame = MidiMessage::UNSCHEDULED;
        m.bytes[0] = status;
        m.len = 1;
        emit(m, MidiMessage::UNSCHEDULED);
    }

    /**
     * A message the CLOCK produced, already due: straight out, not through `pending_`. It keeps its
     * real `frame` so clock lateness stays measurable.
     */
    void emit_bytes(int64_t frame, const uint8_t* bytes, int len, int64_t nowFrame) {
        MidiMessage m;
        m.frame = frame;
        m.seq   = seq_++;
        for (int i = 0; i < len && i < 3; ++i) m.bytes[i] = bytes[i];
        m.len = static_cast<uint8_t>(len);
        emit(m, nowFrame);
    }

    /**
     * Frames per quarter at the live tempo, read fresh every pump (project tempo, as in `note_on`).
     * ⚠️ Built from the truncated `frames_per_step`, so the clock stays on the scheduler's grid.
     */
    int64_t frames_per_quarter() const {
        const int tempo = (project_ && project_->tempo > 0) ? project_->tempo : tempo_;
        return frames_per_step(tempo, sampleRate_) * 4;
    }

    /** The hex number following `key` in a `t_play` detail string ("row=0A" → 10); −1 if absent. */
    static int hex_after(const std::string& detail, const std::string& key) {
        const size_t p = detail.find(key);
        if (p == std::string::npos) return -1;
        int v = 0;
        bool any = false;
        for (size_t i = p + key.size(); i < detail.size(); ++i) {
            const char c = detail[i];
            int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else break;
            v = v * 16 + d;
            any = true;
        }
        return any ? v : -1;
    }

    void emit(const MidiMessage& m, int64_t nowFrame) {
        const bool sent = out_ && out_->is_open();
        if (sent) out_->send(m.bytes, m.len);
        // ⚠️ Outside the `sent` test: timing is measurable with no cable, and "released with no port"
        // differs from "never released".
        if (observer_) observer_->on_released(m, nowFrame, sent);
    }

    void forget_channel_state() {
        for (int c = 0; c < 16; ++c) {
            lastBank_[c] = -2;      // -2, not -1: -1 is a real value ("send nothing")
            lastProgram_[c] = -2;
            lastPan_[c] = -1;
            channelUsed_[c] = false;
            bent_[c] = false;
        }
    }

    // `mutable` so the const readers can lock: a value another thread writes.
    mutable std::mutex mu_;

    const Project*     project_  = nullptr;
    IMidiOut*          out_      = nullptr;
    IMidiSendObserver* observer_ = nullptr;

    TrackInstruments tracks_;
    ActiveNote       active_[TrackInstruments::LANES];
    MidiClock        clock_;   // no lock of its own; guarded by `mu_`

    std::vector<MidiMessage> pending_;
    uint32_t seq_ = 0;

    int tempo_ = 128, sampleRate_ = 44100, offsetMs_ = 0;

    int  lastBank_[16] = {0}, lastProgram_[16] = {0}, lastPan_[16] = {0};
    bool channelUsed_[16] = {false};
    bool bent_[16] = {false};   // channels an MPB moved off centre
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_OUT_H
