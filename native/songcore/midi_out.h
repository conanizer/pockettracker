#ifndef POCKETTRACKER_SONGCORE_MIDI_OUT_H
#define POCKETTRACKER_SONGCORE_MIDI_OUT_H

// The EXTERNAL consumer: bus events become MIDI bytes on a cable — the mirror of engine_consumer.h.
// `IMidiOut` is the platform seam. One active note per track, ended by a LEN gate, the next note, a
// KIL or the transport stopping. ⚠️ Gear has no voice allocator: an unanswered note-on sounds for ever.
// Events arrive ahead of the clock, are queued by frame and released by `pump(now)` — from the sender
// thread (`shell/midi/midi-sender.h`) or `SongcoreHost::poll`. Two threads, hence `mu_`.

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

// A MIDI output port, one per platform. `send` gets a complete message and must not block for long —
// it runs on whichever thread pumps the queue.
struct IMidiOut {
    virtual ~IMidiOut() = default;

    // Re-enumerated on each call; hot-plug changes it.
    virtual int device_count() = 0;
    virtual std::string device_name(int index) = 0;

    virtual bool open(int index) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    virtual void send(const uint8_t* data, int len) = 0;

    // The open port failed in a way only a reopen cures (the cable was pulled).
    virtual bool broken() const { return false; }

    // A synth built into the OS — AUTO passes over it.
    virtual bool is_builtin_synth(int /*index*/) { return false; }
};

// A 0..1 bus value to 0–127 — the one place, so internal and external cannot drift.
inline int to7bit(float v01) {
    if (!(v01 > 0.0f)) return 0;      // also catches NaN
    if (v01 >= 1.0f) return 127;
    return static_cast<int>(v01 * 127.0f + 0.5f);
}

// An authored 0x00–0xFF byte to 0–127.
inline int byte_to_7bit(int b) {
    if (b <= 0) return 0;
    if (b >= 255) return 127;
    return (b * 127 + 127) / 255;
}

constexpr uint8_t MIDI_NOTE_OFF = 0x80, MIDI_NOTE_ON = 0x90, MIDI_CC = 0xB0,
                  MIDI_PROGRAM  = 0xC0, MIDI_PITCH_BEND = 0xE0;

constexpr uint8_t MIDI_CC_BANK_MSB = 0, MIDI_CC_BANK_LSB = 32, MIDI_CC_ALL_NOTES_OFF = 123;

// No bend — where a channel rests, and what a panic restores.
constexpr int MIDI_BEND_CENTRE = 0x2000;

// One message and the frame it is due on. `seq` keeps same-frame messages in production order
// (bank → program → CC → note-on). `UNSCHEDULED` = a panic message; a lateness observer skips it.
struct MidiMessage {
    static constexpr int64_t UNSCHEDULED = INT64_MIN;

    int64_t  frame = 0;
    uint32_t seq   = 0;
    uint8_t  bytes[3] = {0, 0, 0};
    uint8_t  len   = 0;
};

// Told about every message as it is released — how lateness is measured; called with no port open too.
// ⚠️ Called under the lock: be quick, and never call back into `ExternalConsumer`.
struct IMidiSendObserver {
    virtual ~IMidiSendObserver() = default;
    virtual void on_released(const MidiMessage& m, int64_t nowFrame, bool sent) = 0;
};

class ExternalConsumer : public IMidiConsumer {
  public:
    explicit ExternalConsumer(const Project* project) : project_(project) { forget_channel_state(); }
    ~ExternalConsumer() override { panic(); }

    // ⚠️ Threads: the frame loop produces and panics, the sender thread only calls `pump`. Every public
    // method takes `mu_`, every private helper assumes it held (not recursive).
    // The port write is under the lock on purpose: `IMidiOut::send` need not be thread-safe.

    // Attach (or detach, with nullptr) the port.
    // ⚠️ Panics FIRST: after a swap the owed note-offs could not reach the device holding the notes.
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

    // The track→instrument state, for MIDI in's router.
    // ⚠️ Not locked: written and read only on the frame-loop thread.
    const TrackInstruments& track_instruments() const { return tracks_; }

    // Set once at boot, before the sender thread starts.
    void set_send_observer(IMidiSendObserver* obs) {
        std::lock_guard<std::mutex> lk(mu_);
        observer_ = obs;
    }

    // SYNC OUT — the 24 PPQN clock and transport bytes (midi_clock.h). On takes effect at the next start.
    // ⚠️ Off mid-take sends the owed Stop, or a synth on external sync waits for ever.
    void set_sync_out(bool on) {
        std::lock_guard<std::mutex> lk(mu_);
        if (clock_.set_enabled(on)) send_now_1(MIDI_RT_STOP);
    }
    bool sync_out() const {
        std::lock_guard<std::mutex> lk(mu_);
        return clock_.enabled();
    }

    // Speaker-to-cable alignment in ms (positive = MIDI later), applied at send time.
    void set_offset_ms(int ms) {
        std::lock_guard<std::mutex> lk(mu_);
        offsetMs_ = ms;
    }
    int offset_ms() const {
        std::lock_guard<std::mutex> lk(mu_);
        return offsetMs_;
    }

    // ── IMidiConsumer ──

    // The transport starting (`t_play` comes once per take; a render never reaches this consumer).
    // ⚠️ The panic makes `tracks_.reset()` safe: reset with notes active, their note-offs are never sent.
    void on_play(const std::string& kind, const std::string& detail, int64_t startFrame,
                 int tempo, int sample_rate) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (tempo > 0) tempo_ = tempo;
        if (sample_rate > 0) sampleRate_ = sample_rate;
        // A fresh take is a fresh device state.
        panic_locked();
        tracks_.reset();

        // Arm the clock at the take's start; only SONG carries a row (`row=` in `detail`).
        const int startRow = kind == "SONG" ? hex_after(detail, "row=") : 0;
        const int spp      = (startRow > 0 && project_) ? nominal_spp_beats(*project_, startRow) : 0;
        clock_.start(startFrame, frames_per_quarter(), spp);
    }

    // ⚠️ Deliberately empty: `t_stop` ends every scheduling pass, not the transport — a panic here would
    // discard the take as it was queued. The real stop is `SongcoreHost::stop()` → `panic()`.
    void on_stop() override {}

    void consume(const Event& ev) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!project_) return;

        // Learn from EVERY note-on, internal ones too, or a track going EXTERNAL keeps its old instrument.
        const int16_t prev  = tracks_.current(ev.track);
        const int16_t instr = tracks_.observe(ev);

        if (!routes_external(instr)) {
            // ⚠️ A track leaving EXTERNAL owes the device a note-off; nothing later would deliver it.
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
            default:   // EV_EXT_* have no MIDI form
                break;
        }
    }

    // Release everything due at or before `nowFrame` and close expired LEN gates — late, never early.
    void pump(int64_t nowFrame) {
        std::lock_guard<std::mutex> lk(mu_);
        // "Now" in event frames: the clock, LEN gates and the queue all share the OFFSET.
        const int64_t due = nowFrame - offset_frames();
        // The clock first: Start must precede the take.
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

    // All notes off now on every channel touched (per note, then CC 123), and the queue dropped.
    void panic() {
        std::lock_guard<std::mutex> lk(mu_);
        panic_locked();
    }


    size_t pending_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_.size();
    }

    // The sender thread's busy/idle test. ⚠️ The clock queues nothing, so the queue alone would idle the
    // thread during pure sync out. LEN gates are left out (the 4 ms idle bound is inaudible).
    bool needs_fast_pump() const {
        std::lock_guard<std::mutex> lk(mu_);
        return !pending_.empty() || clock_.running();
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
        // Centre bent channels — an MPB with no note still bent the device.
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

    // The routing verdict, from the model so both consumers agree.
    bool routes_external(int16_t instrument) const {
        if (instrument < 0 || !project_) return false;
        if (static_cast<size_t>(instrument) >= project_->instruments.size()) return false;
        return instrument_routes_external(project_->instruments[static_cast<size_t>(instrument)]);
    }

    void note_on(const Event& ev, const Instrument& ins) {
        const uint8_t ch = chan(ins);
        // The previous note ends here — its only end when midiLen is 0.
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
        // ⚠️ The project's tempo, not `on_play`'s fallback, so a LEN gate is the same in play and render.
        const int tempo = project_->tempo > 0 ? project_->tempo : tempo_;
        a.offFrame = ins.midiLen > 0
                         ? ev.frame + ins.midiLen * frames_per_tic(frames_per_step(tempo, sampleRate_))
                         : INT64_MAX;
        channelUsed_[ch] = true;
    }

    // End the track's note at `frame` or its LEN gate, whichever is FIRST.
    // ⚠️ The `min` is what makes LEN work: the next note usually arrives before the gate is due.
    void end_note(uint8_t track, int64_t frame) {
        if (track >= TrackInstruments::LANES) return;
        ActiveNote& a = active_[track];
        if (!a.on) return;
        send_at(std::min(frame, a.offFrame), MIDI_NOTE_OFF | a.channel, a.note, 0);
        a.on = false;
        a.offFrame = INT64_MAX;
    }

    // The transpose fields folded in, clamped: B-9 = 131 is a legal phrase note but not MIDI.
    static int midi_note(const NoteOnPayload& n) {
        const int v = static_cast<int>(n.note) + n.transpose + n.pit + n.arp;
        return v < 0 ? 0 : (v > 127 ? 127 : v);
    }

    // Velocity scaled by the VOL channel — an external instrument's only volume. At least 1: velocity 0
    // IS a note-off.
    static int midi_velocity(const NoteOnPayload& n) {
        const float unit = note_velocity_unit(n) * f32_from_bits(n.volGainBits);
        const int v = to7bit(unit);
        return v < 1 ? 1 : v;
    }

    // Bank + program, when set and the channel is not already on them (forgotten at each start).
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

    // The instrument's CC slots, re-sent with EVERY note-on: a CCA or a knob may have moved them.
    void send_cc_defaults(int64_t frame, uint8_t ch, const Instrument& ins) {
        for (const MidiCcSlot& s : ins.midiCC) {
            if (s.cc < 0 || s.value < 0) continue;
            send_at(frame, MIDI_CC | ch, static_cast<uint8_t>(s.cc & 0x7F),
                    static_cast<uint8_t>(s.value & 0x7F));
        }
    }

    // A bus CC onto the wire. ⚠️ A slot id (128-131) must never reach the `& 0x7F` — 128 masks to BANK
    // SELECT; `resolve_cc_param` translates it.
    void cc_event(const Event& ev, const Instrument& ins) {
        const int param = resolve_cc_param(ins, ev.cc.param);
        if (param < 0) return;    // an unassigned slot: nothing to move
        const uint8_t ch = chan(ins);
        const int v7 = to7bit(f32_from_bits(ev.cc.valueBits));
        send_at(ev.frame, MIDI_CC | ch, static_cast<uint8_t>(param & 0x7F), static_cast<uint8_t>(v7));
        if (param == CC_PAN) lastPan_[ch] = v7;   // keep the note-on de-dup honest
    }

    // MPG. ⚠️ Updates `lastProgram_`, or the next note would stay on the MPG's program; the bank
    // becomes unknown.
    void program_event(const Event& ev, const Instrument& ins) {
        const uint8_t ch = chan(ins);
        const int prog = ev.program.program & 0x7F;
        send_at(ev.frame, MIDI_PROGRAM | ch, static_cast<uint8_t>(prog));
        lastProgram_[ch] = prog;
        lastBank_[ch] = -2;
    }

    // MPB, an absolute 14-bit bend. ⚠️ A bent channel detunes every later note; `panic_locked` centres
    // the ones we bent, and only those.
    void pitch_bend_event(const Event& ev, const Instrument& ins) {
        const uint8_t ch = chan(ins);
        const int v14 = ev.pitchBend.value14 & 0x3FFF;
        send_at(ev.frame, MIDI_PITCH_BEND | ch,
                static_cast<uint8_t>(v14 & 0x7F), static_cast<uint8_t>((v14 >> 7) & 0x7F));
        bent_[ch] = (v14 != MIDI_BEND_CENTRE);
    }

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
        // Sorted insert on (frame, seq); usually at the end.
        auto pos = std::upper_bound(pending_.begin(), pending_.end(), m,
                                    [](const MidiMessage& a, const MidiMessage& b) {
                                        return a.frame != b.frame ? a.frame < b.frame : a.seq < b.seq;
                                    });
        pending_.insert(pos, m);
    }

    // Bypass the queue — panic only.
    void send_now(uint8_t status, uint8_t d1, uint8_t d2) {
        MidiMessage m;
        m.frame = MidiMessage::UNSCHEDULED;   // never had a due time
        m.bytes[0] = status; m.bytes[1] = d1; m.bytes[2] = d2;
        m.len = 3;
        emit(m, MidiMessage::UNSCHEDULED);
    }

    // The one-byte form, for Stop.
    void send_now_1(uint8_t status) {
        MidiMessage m;
        m.frame = MidiMessage::UNSCHEDULED;
        m.bytes[0] = status;
        m.len = 1;
        emit(m, MidiMessage::UNSCHEDULED);
    }

    // A clock message, already due: straight out, keeping its frame so lateness is measurable.
    void emit_bytes(int64_t frame, const uint8_t* bytes, int len, int64_t nowFrame) {
        MidiMessage m;
        m.frame = frame;
        m.seq   = seq_++;
        for (int i = 0; i < len && i < 3; ++i) m.bytes[i] = bytes[i];
        m.len = static_cast<uint8_t>(len);
        emit(m, nowFrame);
    }

    // ⚠️ Built from the truncated `frames_per_step`, so the clock stays on the scheduler's grid.
    int64_t frames_per_quarter() const {
        const int tempo = (project_ && project_->tempo > 0) ? project_->tempo : tempo_;
        return frames_per_step(tempo, sampleRate_) * 4;
    }

    // The hex number after `key` in a `t_play` detail ("row=0A" → 10); −1 if absent.
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
        // Outside the `sent` test, so timing is measurable with no cable.
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
