#ifndef POCKETTRACKER_SONGCORE_MIDI_OUT_H
#define POCKETTRACKER_SONGCORE_MIDI_OUT_H

// The EXTERNAL consumer: bus events become MIDI bytes on a cable — the mirror of engine_consumer.h.
// `IMidiOut` is the platform seam. One active note per track, ended by a LEN gate, the next note, a
// KIL or the transport stopping. ⚠️ Gear has no voice allocator: an unanswered note-on sounds for ever.
// Events arrive ahead of the clock, are queued by frame and released by `pump(now)` — from the sender
// thread (`shell/midi/midi-sender.h`) or `SongcoreHost::poll`. Two threads, hence `mu_`.
// The pitch commands become the channel's pitch wheel (`Wheel`).

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "effects.h"       // fine_tune_semitones
#include "event.h"
#include "note_command.h"
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

// RPN 0, the bend range: the RPN's number goes to 101/100, its value to 6/38.
constexpr uint8_t MIDI_CC_RPN_MSB = 101, MIDI_CC_RPN_LSB = 100, MIDI_CC_DATA_MSB = 6, MIDI_CC_DATA_LSB = 38;

// A moving wheel is re-sent this often. ⚠️ A 5-pin cable carries ~1000 such messages a second in all.
constexpr int MIDI_WHEEL_MS = 5;

// One message and the frame it is due on. `seq` keeps same-frame messages in production order
// (bank → program → CC → note-on). `UNSCHEDULED` = a panic message; a lateness observer skips it.
struct MidiMessage {
    static constexpr int64_t UNSCHEDULED = INT64_MIN;

    int64_t  frame = 0;
    uint32_t seq   = 0;
    uint8_t  bytes[NOTE_COMMAND_BYTES] = {};   // the longest a note command
    uint8_t  len   = 0;
    // A wheel position: its BEND goes first when the channel holds another. A re-send equal to what
    // the channel holds is dropped, unless it is an MPB — a command, sent as written.
    uint8_t  bendRange  = 0;
    bool     bendAlways = false;
    uint8_t  track      = 0xFF;   // the sequencer track that queued it, for a LIVE take-back
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

    // SYNC IN: while another device's clock leads, the tempo and the quarter note this plays at
    // (0 = the song's) — the clock it sends on, its LEN gates and its wheel.
    void set_follow(int bpm, int64_t framesPerQuarter) {
        std::lock_guard<std::mutex> lk(mu_);
        followBpm_     = bpm;
        followQuarter_ = framesPerQuarter;
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

        // Arm the clock at the take's start; only SONG carries a row (`row=`), and a step into it.
        const int startRow = kind == "SONG" ? hex_after(detail, "row=") : 0;
        const int step     = kind == "SONG" ? std::max(0, hex_after(detail, "step=")) : 0;
        const int spp      = (startRow > 0 && project_) ? nominal_spp_beats(*project_, startRow) : 0;
        clock_.start(startFrame, frames_per_quarter(), spp + step);
    }

    // ⚠️ Deliberately empty: `t_stop` ends every scheduling pass, not the transport — a panic here would
    // discard the take as it was queued. The real stop is `SongcoreHost::stop()` → `panic()`.
    void on_stop() override {}

    void consume(const Event& ev) override {
        std::lock_guard<std::mutex> lk(mu_);
        consume_locked(ev);
    }

    // A live key's record, sent thru: its CC 7 is the player's channel volume, which no note undoes.
    void consume_live(const Event& ev) {
        std::lock_guard<std::mutex> lk(mu_);
        live_ = true;
        consume_locked(ev);
        live_ = false;
    }

    // Release everything due at or before `nowFrame` and close expired LEN gates — late, never early.
    void pump(int64_t nowFrame) {
        std::lock_guard<std::mutex> lk(mu_);
        // "Now" in event frames: the clock, LEN gates and the queue all share the OFFSET.
        const int64_t due = nowFrame - offset_frames();
        if (due > lastDue_) lastDue_ = due;
        // The clock first: Start must precede the take.
        clock_.pump(due, frames_per_quarter(), [&](int64_t at, const uint8_t* bytes, int len) {
            emit_bytes(at, bytes, len, nowFrame);
        });
        // Expired gates first, so their note-offs sort into the queue ahead of anything later.
        for (int t = 0; t < TrackInstruments::LANES; ++t) {
            producer_ = static_cast<uint8_t>(t);
            if (active_[t].on && active_[t].offFrame <= due)
                end_note(static_cast<uint8_t>(t), active_[t].offFrame);
            // A moving wheel's re-sends since its last event (the ones before it went out with it).
            run_wheel(static_cast<uint8_t>(t), due + 1);
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

    // A LIVE launch re-schedules `track` from `frame`: what its events from there queued is withdrawn
    // and the track goes back to the note and wheel it had there, so the old pattern's lookahead does
    // not play beside the new one. ⚠️ What is already on the wire stays.
    void rollback(int track, int64_t frame) {
        std::lock_guard<std::mutex> lk(mu_);
        if (track < 0 || track >= POOL_TRACKS) return;
        std::vector<TrackSnap>& h = history_[track];
        const auto it = std::lower_bound(h.begin(), h.end(), frame,
                                         [](const TrackSnap& s, int64_t f) { return s.frame < f; });
        if (it == h.end()) return;   // nothing consumed from there on
        const TrackSnap s = *it;
        h.erase(it, h.end());

        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [&](const MidiMessage& m) {
                                          if (m.track != track || m.seq < s.seq) return false;
                                          forget_sent_state(m);
                                          return true;
                                      }),
                       pending_.end());
        tracks_.set(static_cast<uint8_t>(track), s.instrument);
        active_[track] = s.active;
        // Its LEN gate has passed, so its note-off is already out.
        if (active_[track].offFrame <= lastDue_) active_[track].on = false;
        wheel_[track] = s.wheel;
    }


    size_t pending_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_.size();
    }

    // The sender thread's busy/idle test. ⚠️ The clock queues nothing, so the queue alone would idle the
    // thread during pure sync out. LEN gates are left out (the 4 ms idle bound is inaudible).
    bool needs_fast_pump() const {
        std::lock_guard<std::mutex> lk(mu_);
        if (!pending_.empty() || clock_.running()) return true;
        for (const Wheel& w : wheel_)
            if (w.next != INT64_MAX) return true;
        return false;
    }


  private:
    // ⚠️ `mu_` held. `live_` says whether the record is a live key's.
    void consume_locked(const Event& ev) {
        if (!project_) return;
        remember(ev);
        producer_ = ev.track;

        // Learn from EVERY note-on, internal ones too, or a track going EXTERNAL keeps its old instrument.
        const int16_t prev  = tracks_.current(ev.track);
        const int16_t instr = tracks_.observe(ev);

        if (!routes_external(instr)) {
            // ⚠️ A track leaving EXTERNAL owes the device a note-off; nothing later would deliver it.
            if (ev.type == EV_NOTE_ON && routes_external(prev)) {
                end_note(ev.track, ev.frame);
                wheel_[ev.track].next = INT64_MAX;
            }
            return;
        }
        const Instrument& ins = project_->instruments[static_cast<size_t>(instr)];

        // The wheel's motion up to this event goes out before the event changes it.
        run_wheel(ev.track, ev.frame);

        switch (ev.type) {
            case EV_NOTE_ON:   note_on(ev, ins);  break;
            case EV_NOTE_OFF:  end_note(ev.track, ev.frame); break;   // KIL and ADSR release alike
            case EV_CC:        cc_event(ev, ins); break;
            case EV_PROGRAM: program_event(ev, ins); break;
            case EV_PITCH_BEND: pitch_bend_event(ev, ins); break;
            case EV_EXT_PITCH_RATE: wheel_rate(ev, ins);    break;
            case EV_EXT_VIBRATO:    wheel_vibrato(ev, ins); break;
            default:   // the other EV_EXT_* have no MIDI form
                break;
        }
    }

    void panic_locked() {
        // ⚠️ The Stop first: it stops the device's own sequencer at the same instant we end our notes.
        if (clock_.stop()) send_now_1(MIDI_RT_STOP);
        // ⚠️ From what the device RECEIVED, not from `active_`: that runs a lookahead early, so it can
        // name the next note while the sounding one's note-off is still queued — and dropped below.
        for (int c = 0; c < 16; ++c) {
            for (int n = 0; n < 128; ++n)
                if (soundingOut_[c][static_cast<size_t>(n)])
                    send_now(MIDI_NOTE_OFF | static_cast<uint8_t>(c), static_cast<uint8_t>(n), 0);
        }
        for (ActiveNote& a : active_) a.on = false;
        for (int c = 0; c < 16; ++c) {
            if (!channelUsed_[c]) continue;
            send_now(MIDI_CC | static_cast<uint8_t>(c), MIDI_CC_ALL_NOTES_OFF, 0);
        }
        // Centre bent channels — a bend outlives its note — and turn a VOL's channel back up.
        for (int c = 0; c < 16; ++c) {
            if (wheelOut_[c] == MIDI_BEND_CENTRE) continue;
            send_now(MIDI_PITCH_BEND | static_cast<uint8_t>(c),
                     MIDI_BEND_CENTRE & 0x7F, (MIDI_BEND_CENTRE >> 7) & 0x7F);
        }
        for (int c = 0; c < 16; ++c)
            if (volOut_[c] != VOL_UNTOUCHED && volOut_[c] != 127)
                send_now(MIDI_CC | static_cast<uint8_t>(c), CC_VOLUME, 127);
        pending_.clear();
        for (Wheel& w : wheel_) w = Wheel{};
        for (std::vector<TrackSnap>& h : history_) h.clear();
        forget_channel_state();
    }

    // The track's state before `ev`, for `rollback`. What the pump has passed can no longer be
    // taken back, so it is dropped.
    void remember(const Event& ev) {
        if (ev.track >= POOL_TRACKS) return;
        std::vector<TrackSnap>& h = history_[ev.track];
        size_t keep = 0;
        while (keep < h.size() && h[keep].frame < lastDue_) ++keep;
        if (h.size() - keep >= HISTORY_CAP) keep = h.size() - HISTORY_CAP + 1;   // a pump that never runs
        if (keep > 0) h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(keep));
        h.push_back(TrackSnap{ev.frame, seq_, tracks_.current(ev.track), active_[ev.track], wheel_[ev.track]});
    }

    // A withdrawn message never reached the device, so the de-dup that skipped re-sending it is wrong.
    void forget_sent_state(const MidiMessage& m) {
        const uint8_t kind = m.bytes[0] & 0xF0, ch = m.bytes[0] & 0x0F;
        if (kind == MIDI_PROGRAM ||
            (kind == MIDI_CC && (m.bytes[1] == MIDI_CC_BANK_MSB || m.bytes[1] == MIDI_CC_BANK_LSB))) {
            lastBank_[ch] = -2;
            lastProgram_[ch] = -2;
        }
        if (kind == MIDI_CC && m.bytes[1] == CC_PAN) lastPan_[ch] = -1;
        if (kind == MIDI_CC && m.bytes[1] == CC_VOLUME) volOut_[ch] = VOL_UNKNOWN;
    }

    struct ActiveNote {
        bool    on       = false;
        uint8_t channel  = 0;
        uint8_t note     = 0;            // a chord's lowest
        uint8_t chord[3] = {};           // a chord's other notes, each with its own note-off
        uint8_t chordCount = 0;
        int64_t keyOn[4] = {};           // when `note`, then each of `chord`, goes on (a strum)
        int64_t onFrame  = 0;
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
        // The instrument's VOL rides the velocity, so instruments sharing a channel keep their balance.
        // A VOL command or fade replaces it, as in the app's own sound, and goes out as CC 7 ahead of
        // the note — so the note starts at it whatever the device does with velocity.
        const float vol    = f32_from_bits(ev.noteOn.volGainBits);
        const bool  volCmd = vol != hex_to_float(ins.volume);
        state_volume(ev.frame, ch, volCmd ? to7bit(vol) : 127);
        send_cc_defaults(ev.frame, ch, ins);

        // PAN rides CC 10, sent only when it changes.
        const int pan7 = to7bit(f32_from_bits(ev.noteOn.panBits));
        if (lastPan_[ch] != pan7) {
            send_at(ev.frame, MIDI_CC | ch, CC_PAN, static_cast<uint8_t>(pan7));
            lastPan_[ch] = pan7;
        }

        // Ahead of the note, so a PSL's attack starts on the old pitch.
        start_wheel(ev, ins);

        ActiveNote& a = active_[ev.track];
        a.on = true;
        a.channel = ch;
        a.onFrame = ev.frame;
        // ⚠️ The project's tempo, not `on_play`'s fallback, so a LEN gate is the same in play and render.
        const int tempo = tempo_now();
        a.offFrame = ins.midiLen > 0
                         ? ev.frame + ins.midiLen * frames_per_tic(frames_per_step(tempo, sampleRate_))
                         : INT64_MAX;

        // A strummed key whose turn comes after the LEN gate is never sent.
        int keys[4];
        int64_t wait[4];
        const int keyCount = midi_keys(ev.noteOn, keys, wait);
        const int vel  = midi_velocity(ev.noteOn, volCmd ? 1.0f : vol);
        a.chordCount = 0;
        for (int k = 0; k < keyCount; ++k) {
            const int64_t on = ev.frame + wait[k];
            if (k > 0 && on >= a.offFrame) continue;
            send_at(on, MIDI_NOTE_ON | ch, static_cast<uint8_t>(keys[k]), static_cast<uint8_t>(vel));
            if (k == 0) {
                a.note = static_cast<uint8_t>(keys[0]);
                a.keyOn[0] = on;
            } else {
                a.chord[a.chordCount] = static_cast<uint8_t>(keys[k]);
                a.keyOn[++a.chordCount] = on;
            }
        }
        channelUsed_[ch] = true;
    }

    // End the track's note at `frame` or its LEN gate, whichever is FIRST.
    // ⚠️ The `min` is what makes LEN work: the next note usually arrives before the gate is due.
    void end_note(uint8_t track, int64_t frame) {
        if (track >= TrackInstruments::LANES) return;
        ActiveNote& a = active_[track];
        if (!a.on) return;
        const int64_t off = std::min(frame, a.offFrame);
        key_off(track, a, a.note, a.keyOn[0], off);
        for (int k = 0; k < a.chordCount; ++k) key_off(track, a, a.chord[k], a.keyOn[k + 1], off);
        a.on = false;
        a.offFrame = INT64_MAX;
    }

    // One key's note-off. A strummed key still waiting for its turn is taken back instead.
    void key_off(uint8_t track, const ActiveNote& a, uint8_t key, int64_t on, int64_t off) {
        if (on > off) {
            const uint8_t status = MIDI_NOTE_ON | a.channel;
            const auto it = std::find_if(pending_.begin(), pending_.end(), [&](const MidiMessage& m) {
                return m.track == track && m.frame == on && m.bytes[0] == status && m.bytes[1] == key;
            });
            if (it != pending_.end()) {
                pending_.erase(it);
                return;
            }
        }
        send_at(std::max(on, off), MIDI_NOTE_OFF | a.channel, key, 0);
    }

    // The transpose fields folded in, clamped: B-9 = 131 is a legal phrase note but not MIDI.
    static int midi_note(const NoteOnPayload& n) {
        const int v = static_cast<int>(n.note) + n.transpose + n.pit + n.arp;
        return v < 0 ? 0 : (v > 127 ? 127 : v);
    }

    // Every key the note plays: itself, or a chord's notes — clamped, and two clamped onto one are one,
    // at the earlier turn. `wait` = frames after the note-on, ascending: the first key is the first heard.
    static int midi_keys(const NoteOnPayload& n, int out[4], int64_t wait[4]) {
        if (n.chordCount == 0) {
            out[0]  = midi_note(n);
            wait[0] = 0;
            return 1;
        }
        const int base = static_cast<int>(n.note) + n.transpose + n.pit + n.arp;
        int count = 0;
        for (int k = 0; k < n.chordCount && k < 4; ++k) {
            const int v = base + n.chord[k];
            const int key = v < 0 ? 0 : (v > 127 ? 127 : v);
            const int64_t w = static_cast<int64_t>(n.strumOrder[k]) * n.strumStep;
            if (count > 0 && out[count - 1] == key) {
                wait[count - 1] = std::min(wait[count - 1], w);
                continue;
            }
            out[count]    = key;
            wait[count++] = w;
        }
        for (int i = 1; i < count; ++i)
            for (int j = i; j > 0 && wait[j - 1] > wait[j]; --j) {
                std::swap(wait[j - 1], wait[j]);
                std::swap(out[j - 1], out[j]);
            }
        return count;
    }

    // The V column, scaled by the instrument's VOL. At least 1: velocity 0 IS a note-off.
    static int midi_velocity(const NoteOnPayload& n, float volume) {
        const int v = to7bit(note_velocity_unit(n) * volume);
        return v < 1 ? 1 : v;
    }

    // CC 7 as the note needs it. A channel no VOL has touched counts as full, so a song without VOL
    // puts no CC 7 on the wire.
    void state_volume(int64_t frame, uint8_t ch, int v7) {
        if (volOut_[ch] == v7 || (volOut_[ch] == VOL_UNTOUCHED && v7 == 127)) return;
        send_at(frame, MIDI_CC | ch, CC_VOLUME, static_cast<uint8_t>(v7));
        volOut_[ch] = v7;
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
        if (ev.cc.param == CC_FINE_TUNE) {
            wheel_fine(ev, ins);
            return;
        }
        if (travels_as_note_command(ev.cc.param)) {
            note_command(ev);
            return;
        }
        const int param = resolve_cc_param(ins, ev.cc.param);
        if (param < 0) return;    // an unassigned slot: nothing to move
        const uint8_t ch = chan(ins);
        const int v7 = to7bit(f32_from_bits(ev.cc.valueBits));
        send_at(ev.frame, MIDI_CC | ch, static_cast<uint8_t>(param & 0x7F), static_cast<uint8_t>(v7));
        if (param == CC_PAN) lastPan_[ch] = v7;   // keep the note-on de-dup honest
        if (ev.cc.param == CC_VOLUME) volOut_[ch] = live_ ? VOL_UNTOUCHED : v7;
    }

    // A command a phrase gave the track's note, for another PocketTracker: with the note while its
    // note-on is still queued — a step's own commands come a frame behind it — else for the note
    // sounding. Nothing sounding, nothing to change, as in the app's own sound.
    void note_command(const Event& ev) {
        if (ev.track >= TrackInstruments::LANES || !active_[ev.track].on) return;
        const ActiveNote& a = active_[ev.track];
        NoteCommand c;
        c.channel = a.channel;
        c.note    = a.note;
        c.id      = ev.cc.param;
        c.value   = static_cast<uint8_t>(std::lround(f32_from_bits(ev.cc.valueBits) * 255.0f));
        const auto on = std::find_if(pending_.begin(), pending_.end(), [&](const MidiMessage& m) {
            return m.track == ev.track && m.frame == a.onFrame && (m.bytes[0] & 0xF0) == MIDI_NOTE_ON;
        });
        const bool withNote = on != pending_.end() && ev.frame <= a.onFrame + 1;
        c.kind = withNote ? NOTE_COMMAND_WITH_NOTE : NOTE_COMMAND_NOW;
        uint8_t bytes[NOTE_COMMAND_BYTES];
        encode_note_command(c, bytes);
        if (!withNote) {
            insert(message(ev.frame, bytes, NOTE_COMMAND_BYTES));
            return;
        }
        MidiMessage m = message(a.onFrame, bytes, NOTE_COMMAND_BYTES);
        m.seq = on->seq;   // rolled back with its note
        pending_.insert(on, m);
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

    // MPB: where the wheel rests under the commands. It outlives the note, until the next MPB or stop.
    void pitch_bend_event(const Event& ev, const Instrument& ins) {
        Wheel& w = wheel_at(ev.track, ins, ev.frame);
        w.mpb = (ev.pitchBend.value14 & 0x3FFF) - MIDI_BEND_CENTRE;
        wheel_changed(w, ev.frame, /*always=*/true);
    }

    // ── The pitch wheel ──
    // A track's PSL / PBN slide, PVB / PVX vibrato, FIN and MPB, run as a sampler voice runs them
    // (engine-mix.cpp `startNotePitchFx`) and sent as the channel's bend, scaled to the instrument's
    // BEND. A closed form from `from`, so a value does not depend on how often `pump` ran.
    // ⚠️ The wheel is per CHANNEL on the device: tracks sharing a channel share every bend.
    struct Wheel {
        uint8_t channel = 0;
        int     range   = MIDI_BEND_RANGE_DEFAULT;  // semitones at full throw
        int64_t from    = 0;
        float   offset  = 0.0f;                     // the slide, in semitones at `from`…
        float   target  = 0.0f, rate = 0.0f;        // …moving toward `target`, semitones a frame
        double  phase   = 0.0;                      // the vibrato: radians at `from`…
        float   speed   = 0.0f, depth = 0.0f;       // …Hz, semitones; depth 0 = none, phase held
        float   fine    = 0.0f;                     // FIN, semitones
        int     mpb     = 0;                        // MPB, wheel steps off centre; outlives the note
        int64_t next    = INT64_MAX;                // when it is next re-sent; INT64_MAX = still
    };

    static float slide_at(const Wheel& w, int64_t frame) {
        if (w.rate == 0.0f) return w.offset;
        const double dt = static_cast<double>(std::max<int64_t>(0, frame - w.from));
        const double v  = w.offset + static_cast<double>(w.rate) * dt;
        return static_cast<float>(w.rate > 0.0f ? std::min(v, static_cast<double>(w.target))
                                                : std::max(v, static_cast<double>(w.target)));
    }

    double phase_at(const Wheel& w, int64_t frame) const {
        constexpr double TWO_PI = 6.283185307179586;
        if (w.depth <= 0.0f) return w.phase;
        const double dt = static_cast<double>(std::max<int64_t>(0, frame - w.from));
        return std::fmod(w.phase + TWO_PI * w.speed * dt / sampleRate_, TWO_PI);
    }

    int wheel_value(const Wheel& w, int64_t frame) const {
        float semis = slide_at(w, frame) + w.fine;
        if (w.depth > 0.0f) semis += static_cast<float>(std::sin(phase_at(w, frame))) * w.depth;
        const long v = std::lround(MIDI_BEND_CENTRE + w.mpb + semis / static_cast<float>(w.range) * 8192.0f);
        return static_cast<int>(std::clamp(v, 0L, 16383L));
    }

    // The anchor moves to `frame`; the motion goes on from where it has got to.
    void settle(Wheel& w, int64_t frame) const {
        if (frame <= w.from) return;
        w.offset = slide_at(w, frame);
        if (w.offset == w.target) w.rate = 0.0f;
        w.phase = phase_at(w, frame);
        w.from  = frame;
    }

    // Moving, unless the slide has landed or is pinned at the wheel's end, and there is no vibrato.
    bool wheel_moves(const Wheel& w, int64_t frame) const {
        if (w.depth > 0.0f) return true;
        if (w.rate == 0.0f || slide_at(w, frame) == w.target) return false;
        const int v = wheel_value(w, frame);
        return w.rate > 0.0f ? v < 16383 : v > 0;
    }

    // The next re-send after one at `frame`. A slide's landing is a point of its own, so it is exact.
    void plan_next(Wheel& w, int64_t frame) const {
        if (!wheel_moves(w, frame)) {
            w.next = INT64_MAX;
            return;
        }
        int64_t next = frame + wheel_step();
        if (w.rate != 0.0f) {
            const int64_t land = frame + static_cast<int64_t>(std::ceil((w.target - slide_at(w, frame)) / w.rate));
            if (land > frame && land < next) next = land;
        }
        w.next = next;
    }

    int64_t wheel_step() const {
        return std::max<int64_t>(1, static_cast<int64_t>(sampleRate_) * MIDI_WHEEL_MS / 1000);
    }

    void queue_wheel(const Wheel& w, int64_t frame, bool always = false) {
        const int v = wheel_value(w, frame);
        queue(frame, MIDI_PITCH_BEND | w.channel, static_cast<uint8_t>(v & 0x7F),
              static_cast<uint8_t>((v >> 7) & 0x7F), 3, static_cast<uint8_t>(w.range), always);
    }

    // Queue the re-sends due before `until`. A backlog the clock has left behind collapses into its
    // newest re-send: a stale position is worse than none, and it is what bounds a late `pump`.
    void run_wheel(uint8_t track, int64_t until) {
        if (track >= TrackInstruments::LANES) return;
        Wheel& w = wheel_[track];
        const int64_t step = wheel_step(), passed = std::min(until - 1, lastDue_);
        if (w.next < until && w.next <= passed && passed - w.next >= step) {
            const int64_t at = w.next + (passed - w.next) / step * step;
            queue_wheel(w, at);
            plan_next(w, at);
        }
        while (w.next < until) {
            const int64_t at = w.next;
            queue_wheel(w, at);
            plan_next(w, at);
        }
    }

    // The track's wheel, aimed at the instrument playing there and anchored at `frame`.
    Wheel& wheel_at(uint8_t track, const Instrument& ins, int64_t frame) {
        Wheel& w = wheel_[track];
        settle(w, frame);
        w.channel = chan(ins);
        w.range   = std::clamp(ins.midiBendRange, MIDI_BEND_RANGE_MIN, MIDI_BEND_RANGE_MAX);
        return w;
    }

    void wheel_changed(Wheel& w, int64_t frame, bool always = false) {
        queue_wheel(w, frame, always);
        plan_next(w, frame);
    }

    // A note starts everything over but MPB, as a voice does.
    void start_wheel(const Event& ev, const Instrument& ins) {
        Wheel& w = wheel_[ev.track];
        const int mpb = w.mpb;
        w = Wheel{};
        w.mpb  = mpb;
        w.from = ev.frame;
        wheel_at(ev.track, ins, ev.frame);

        const NoteOnPayload& n = ev.noteOn;
        const int   tempo = tempo_now();
        const float tic   = frames_per_tic_f(tempo, sampleRate_);
        const float pslOff = f32_from_bits(n.pslOffBits), pslDur = f32_from_bits(n.pslDurBits);
        if (std::fabs(pslOff) > 0.001f && pslDur > 0.0f) {
            w.offset = pslOff;
            w.rate   = -pslOff / std::max(1.0f, pslDur * tic);
        }
        // PBN, semitones a step, takes a PSL's slide over from its offset.
        const float pbn = f32_from_bits(n.pbnRateBits);
        if (pbn != 0.0f) {
            w.rate   = pbn / (tic * TICS_PER_STEP);
            w.target = pbn > 0.0f ? 127.0f : -127.0f;
        }
        const float depth = f32_from_bits(n.vibDepBits);
        if (depth > 0.01f) {
            w.speed = f32_from_bits(n.vibSpdBits);
            w.depth = depth;
        }
        wheel_changed(w, ev.frame);
    }

    // PBN on an empty step: a new rate from where the slide has got to; 00 holds it there.
    void wheel_rate(const Event& ev, const Instrument& ins) {
        Wheel& w = wheel_at(ev.track, ins, ev.frame);
        const float perStep = f32_from_bits(ev.extPitchRate.rateBits);
        if (perStep == 0.0f) {
            w.rate = 0.0f;
        } else {
            const int tempo = ev.extPitchRate.tempo > 0 ? ev.extPitchRate.tempo : tempo_;
            w.rate   = perStep / (frames_per_tic_f(tempo, sampleRate_) * TICS_PER_STEP);
            w.target = perStep > 0.0f ? 127.0f : -127.0f;
        }
        wheel_changed(w, ev.frame);
    }

    // PVB / PVX on an empty step: the phase runs on; 00 stops the vibrato.
    void wheel_vibrato(const Event& ev, const Instrument& ins) {
        Wheel& w = wheel_at(ev.track, ins, ev.frame);
        const float depth = f32_from_bits(ev.extVibrato.depthBits);
        if (depth < 0.01f) {
            w.depth = 0.0f;
        } else {
            w.speed = f32_from_bits(ev.extVibrato.speedBits);
            w.depth = depth;
        }
        wheel_changed(w, ev.frame);
    }

    // FIN retunes the sounding note; the next note clears it.
    void wheel_fine(const Event& ev, const Instrument& ins) {
        Wheel& w = wheel_at(ev.track, ins, ev.frame);
        const int byte = std::clamp(static_cast<int>(f32_from_bits(ev.cc.valueBits) * 255.0f + 0.5f), 0, 255);
        w.fine = fine_tune_semitones(byte);
        wheel_changed(w, ev.frame);
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

    void queue(int64_t frame, uint8_t status, uint8_t d1, uint8_t d2, uint8_t len,
               uint8_t bendRange = 0, bool bendAlways = false) {
        const uint8_t bytes[3] = {status, d1, d2};
        MidiMessage m = message(frame, bytes, len);
        m.bendRange  = bendRange;
        m.bendAlways = bendAlways;
        insert(m);
    }

    MidiMessage message(int64_t frame, const uint8_t* bytes, int len) {
        MidiMessage m;
        m.frame = frame;
        m.seq   = seq_++;
        std::copy(bytes, bytes + len, m.bytes);
        m.len   = static_cast<uint8_t>(len);
        m.track = producer_;
        return m;
    }

    // Sorted on (frame, seq); usually at the end.
    void insert(const MidiMessage& m) {
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

    // ⚠️ Built from the truncated `frames_per_step`, so the clock stays on the scheduler's grid — and
    // from the clock's step while one leads, which the walk is on then.
    int64_t frames_per_quarter() const {
        if (followQuarter_ > 0) return followQuarter_;
        return frames_per_step(tempo_now(), sampleRate_) * 4;
    }

    // The tempo this plays at: a leading clock's, else the song's (`on_play`'s with no project).
    int tempo_now() const {
        if (followBpm_ > 0) return followBpm_;
        return (project_ && project_->tempo > 0) ? project_->tempo : tempo_;
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
        const bool isBend = (m.bytes[0] & 0xF0) == MIDI_PITCH_BEND && m.len == 3;
        const int  ch     = m.bytes[0] & 0x0F;
        const int  value  = m.bytes[1] | (m.bytes[2] << 7);
        if (m.bendRange > 0) {
            // Per CHANNEL, at send time: a plain note re-centres a channel another track left bent.
            if (value == wheelOut_[ch] && !m.bendAlways) return;
            if (rangeOut_[ch] != m.bendRange) send_range(m, nowFrame);
        }
        const bool sent = out_ && out_->is_open();
        if (sent) out_->send(m.bytes, m.len);
        if (isBend) wheelOut_[ch] = value;
        if (sent) track_sounding(m);
        // Outside the `sent` test, so timing is measurable with no cable.
        if (observer_) observer_->on_released(m, nowFrame, sent);
    }

    // The notes the device holds, per channel, as the bytes left.
    void track_sounding(const MidiMessage& m) {
        const uint8_t kind = m.bytes[0] & 0xF0, ch = m.bytes[0] & 0x0F;
        if (kind == MIDI_NOTE_ON && m.bytes[2] > 0) soundingOut_[ch].set(m.bytes[1] & 0x7F);
        else if (kind == MIDI_NOTE_ON || kind == MIDI_NOTE_OFF) soundingOut_[ch].reset(m.bytes[1] & 0x7F);
        else if (kind == MIDI_CC && m.bytes[1] == MIDI_CC_ALL_NOTES_OFF) soundingOut_[ch].reset();
    }

    // RPN 0: the channel's bend range in semitones, then the null RPN, so a CC slot set to 6 later
    // cannot move it.
    void send_range(const MidiMessage& bend, int64_t nowFrame) {
        const uint8_t cc = MIDI_CC | (bend.bytes[0] & 0x0F);
        const uint8_t rpn[6][2] = {{MIDI_CC_RPN_MSB, 0},   {MIDI_CC_RPN_LSB, 0},
                                   {MIDI_CC_DATA_MSB, bend.bendRange}, {MIDI_CC_DATA_LSB, 0},
                                   {MIDI_CC_RPN_MSB, 127}, {MIDI_CC_RPN_LSB, 127}};
        for (const auto& r : rpn) {
            MidiMessage m;
            m.frame = bend.frame;
            m.seq   = bend.seq;
            m.bytes[0] = cc; m.bytes[1] = r[0]; m.bytes[2] = r[1];
            m.len   = 3;
            emit(m, nowFrame);
        }
        rangeOut_[bend.bytes[0] & 0x0F] = bend.bendRange;
    }

    void forget_channel_state() {
        for (int c = 0; c < 16; ++c) {
            lastBank_[c] = -2;      // -2, not -1: -1 is a real value ("send nothing")
            lastProgram_[c] = -2;
            lastPan_[c] = -1;
            channelUsed_[c] = false;
            wheelOut_[c] = MIDI_BEND_CENTRE;
            rangeOut_[c] = 0;       // unknown: the next bend states it
            volOut_[c]   = VOL_UNTOUCHED;
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
    int     followBpm_     = 0;   // set_follow's
    int64_t followQuarter_ = 0;

    int  lastBank_[16] = {0}, lastProgram_[16] = {0}, lastPan_[16] = {0};
    bool channelUsed_[16] = {false};
    int  wheelOut_[16] = {0};   // the bend each channel last received
    // The CC 7 each channel holds from a VOL: a value, or one of these two.
    static constexpr int VOL_UNTOUCHED = -1, VOL_UNKNOWN = -2;
    int  volOut_[16] = {0};
    int  rangeOut_[16] = {0};   // the BEND it was last told, 0 = not yet this take
    std::bitset<128> soundingOut_[16];   // the notes each channel holds — what a panic ends

    Wheel   wheel_[TrackInstruments::LANES];
    int64_t lastDue_ = INT64_MIN;   // the newest "now" `pump` has seen

    // A sequencer track's state before one of its events; `seq` is the first message queued from there.
    struct TrackSnap {
        int64_t    frame;
        uint32_t   seq;
        int16_t    instrument;
        ActiveNote active;
        Wheel      wheel;
    };
    static constexpr size_t HISTORY_CAP = 1024;
    std::vector<TrackSnap> history_[POOL_TRACKS];   // per track, in frame order
    uint8_t producer_ = 0xFF;   // the track whose event or gate is queuing now — stamped on each message
    bool    live_     = false;  // the record being consumed is a live key's (`consume_live`)
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_OUT_H
