#ifndef POCKETTRACKER_SONGCORE_MIDI_OUT_H
#define POCKETTRACKER_SONGCORE_MIDI_OUT_H

// ─── The EXTERNAL consumer — bus events become MIDI bytes ────────────────────────────────────────
//
// The mirror image of engine_consumer.h. That one turns a bus record into engine calls; this one turns
// the SAME record into bytes on a cable, and neither knows the other exists. MIDI plan §4.3.
//
// ⚠️ THIS FILE IS PLATFORM-FREE, and the seam that keeps it so is `IMidiOut` — five methods, the
// smallest surface that can list, open and write to a port. Every platform difference lives behind it
// (Linux: ALSA rawmidi; Windows: winmm; Android: a JNI up-call into `MidiManager`, which is the only
// sanctioned route to USB/BLE/virtual devices there — see the plan §4.3 for why AMidi does not help).
// That is the same shape `AudioBackend` has, for the same reason.
//
// ── WHAT IS DELIBERATELY IN ONE PLACE ────────────────────────────────────────────────────────────
//
//   • **the 0–255/0–1 ↔ 0–127 scaling** (`to7bit`). The plan names it a risk in §11: internal params
//     are wider than the wire, and a second conversion written somewhere else is how internal and
//     external behaviour drift apart without either looking wrong.
//   • **the routing verdict** (`instrument_routes_external` + `TrackInstruments`, router.h). Both
//     consumers ask the same question of the same state.
//   • **the note lifecycle.** One active note per track, always, whatever ends it — a LEN gate, the
//     next note, a KIL, or the transport stopping. An external synth has no voice allocator to save
//     us: a note-on we fail to answer sounds until the power is cut.
//
// ── TIMING, AND WHAT IS NOT SOLVED HERE ──────────────────────────────────────────────────────────
//
// Events arrive at LOOKAHEAD time — the scheduler runs two phrases (~4 s) ahead — so they are queued
// against their target frame and released by `pump(now)`. `pump` is called from wherever the frame
// clock is read; today that is the host's 60 Hz poll, which quantises a send to ~16 ms and is NOT
// good enough for a clock (phase C) and only just good enough for notes. The seam is shaped for the
// fix: `pump` takes its `now` as an argument, so moving it onto a just-in-time sender thread changes
// this file not at all. Until then the jitter is real and documented rather than hidden.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "event.h"
#include "model.h"
#include "note_tables.h"   // f32_from_bits
#include "router.h"
#include "timing.h"

namespace songcore {

// ─── The platform seam ──────────────────────────────────────────────────────────────────────────

/**
 * A MIDI output port. Implemented once per platform; nothing above this interface is per-platform.
 *
 * `send` is called with a complete message (2 or 3 bytes) and must not block for long — it runs on
 * whatever thread pumps the queue.
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
};

// ─── Scaling — the ONE place a wide internal value becomes a 7-bit one (plan §11) ────────────────

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

/** Controller numbers this file sends by name (the plan's §6 map). */
constexpr uint8_t MIDI_CC_BANK_MSB = 0, MIDI_CC_BANK_LSB = 32, MIDI_CC_ALL_NOTES_OFF = 123;

/**
 * One message, stamped with the frame it is due on.
 *
 * `seq` is the arrival counter and exists only to make the queue's order TOTAL: two messages due on
 * the same frame must leave in the order they were produced (bank → program → CC → note-on is
 * meaningless otherwise), and `std::sort` on frame alone would be free to swap them.
 */
struct MidiMessage {
    int64_t  frame = 0;
    uint32_t seq   = 0;
    uint8_t  bytes[3] = {0, 0, 0};
    uint8_t  len   = 0;
};

// ─── The consumer ───────────────────────────────────────────────────────────────────────────────

class ExternalConsumer : public IMidiConsumer {
  public:
    explicit ExternalConsumer(const Project* project) : project_(project) { forget_channel_state(); }
    ~ExternalConsumer() override { panic(); }

    /**
     * Attach (or detach, with nullptr) the port.
     *
     * ⚠️ Panics FIRST. Swapping the cable out from under sounding notes is the one moment where the
     * note-offs we owe can no longer be delivered — after the swap the new port has never heard the
     * note-ons, and the old device holds them forever.
     */
    void set_out(IMidiOut* out) {
        if (out == out_) return;
        panic();
        out_ = out;
    }
    IMidiOut* out() const { return out_; }

    /**
     * The user's alignment between the audio the speakers produce and the notes the cable carries
     * (plan §4.3). Positive = MIDI later. Milliseconds, applied as frames at send time.
     */
    void set_offset_ms(int ms) { offsetMs_ = ms; }
    int  offset_ms() const { return offsetMs_; }

    // ── IMidiConsumer ────────────────────────────────────────────────────────────────────────────

    void on_play(const std::string&, const std::string&, int64_t, int tempo, int sample_rate) override {
        if (tempo > 0) tempo_ = tempo;
        if (sample_rate > 0) sampleRate_ = sample_rate;
        // A fresh transport is a fresh device state: the bank/program/pan we believe each channel is
        // on was true of the last take, and the user may have turned knobs on the gear since.
        forget_channel_state();
        tracks_.reset();
    }

    /**
     * ⚠️ **DELIBERATELY EMPTY — `on_stop` is NOT "the transport stopped".**
     *
     * It is the bus's SEGMENT framing: `t_stop` closes a PLAY..STOP span for the trace consumer, and
     * the scheduler emits one at the end of every scheduling pass — including `scheduleSongRowRange`,
     * which walks a whole song synchronously and is not a performance ending at all. Panicking here
     * threw away the entire queued take the instant it had been scheduled, and the only thing that
     * reached the cable was the panic's own note-offs.
     *
     * The transport stopping is `SongcoreHost::stop()`, which calls `panic()` explicitly. One caller,
     * one meaning.
     */
    void on_stop() override {}

    void consume(const Event& ev) override {
        if (!project_) return;

        // Learn the track→instrument mapping from EVERY note-on, including internal ones — otherwise
        // a track that goes SAMPLER → EXTERNAL would still resolve to the old instrument.
        const int16_t prev  = tracks_.current(ev.track);
        const int16_t instr = tracks_.observe(ev);

        if (!routes_external(instr)) {
            // ⚠️ The mirror of the kill EngineConsumer does when a track flips the other way: a track
            // that leaves EXTERNAL owes the device a note-off for whatever it left sounding, and no
            // later event on that track will ever resolve to us again to deliver it.
            if (ev.type == EV_NOTE_ON && routes_external(prev)) end_note(ev.track, ev.frame);
            return;
        }
        const Instrument& ins = project_->instruments[static_cast<size_t>(instr)];

        switch (ev.type) {
            case EV_NOTE_ON:   note_on(ev, ins);  break;
            case EV_NOTE_OFF:  end_note(ev.track, ev.frame); break;   // KIL and ADSR release alike
            case EV_CC:        cc_event(ev, ins); break;
            case EV_PROGRAM:
                send_at(ev.frame, MIDI_PROGRAM | chan(ins), ev.program.program & 0x7F);
                break;
            case EV_PITCH_BEND:
                send_at(ev.frame, MIDI_PITCH_BEND | chan(ins),
                        ev.pitchBend.value14 & 0x7F, (ev.pitchBend.value14 >> 7) & 0x7F);
                break;
            default:
                // EV_EXT_* — sample start, slice, table row, reverse, EQ. No MIDI 1.0 form exists
                // (event.h flags them EXT for exactly this), so the serializer drops them. That is
                // the rule the schema is built on, not a gap: an EXT event is internal by definition.
                break;
        }
    }

    // ── The clock side ───────────────────────────────────────────────────────────────────────────

    /**
     * Release everything due at or before `nowFrame`, and close any LEN gate that has expired.
     *
     * Late, never early: a message is sent on the first pump at or after its due frame. With a 60 Hz
     * caller that is 0–16 ms of lateness on every event, uniformly — which is why the OFFSET row
     * exists, and why a sender thread is the phase-C prerequisite for clock.
     */
    void pump(int64_t nowFrame) {
        // ONE timeline for both halves: `due` is "now" expressed in EVENT frames, so a LEN gate and a
        // queued message are released by the same clock. Checking the gate against the raw frame and
        // the queue against the offset one would make OFFSET silently mean two different things.
        const int64_t due = nowFrame - offset_frames();
        // Expired gates first, so their note-offs sort into the queue ahead of anything later.
        for (int t = 0; t < TrackInstruments::LANES; ++t) {
            if (active_[t].on && active_[t].offFrame <= due)
                end_note(static_cast<uint8_t>(t), active_[t].offFrame);
        }
        size_t i = 0;
        while (i < pending_.size() && pending_[i].frame <= due) {
            emit(pending_[i]);
            ++i;
        }
        if (i > 0) pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    /**
     * All notes off, right now, on every channel we have touched — and the pending queue dropped.
     *
     * ⚠️ Both halves are load-bearing. The per-note offs are what a device needs to stop the notes it
     * knows about; the CC 123 is the backstop for the ones our bookkeeping has lost (a hot-plug, a
     * crash mid-take, a device that missed a byte). And the queue must go: it holds note-ons for a
     * transport that no longer exists, and releasing them after a stop is a stuck note by definition.
     */
    void panic() {
        for (int t = 0; t < TrackInstruments::LANES; ++t) {
            if (!active_[t].on) continue;
            send_now(MIDI_NOTE_OFF | active_[t].channel, active_[t].note, 0);
            active_[t].on = false;
        }
        for (int c = 0; c < 16; ++c) {
            if (!channelUsed_[c]) continue;
            send_now(MIDI_CC | static_cast<uint8_t>(c), MIDI_CC_ALL_NOTES_OFF, 0);
        }
        pending_.clear();
        forget_channel_state();
    }

    /** Pending messages not yet released — the tools' window into the queue. */
    size_t pending_count() const { return pending_.size(); }

  private:
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
        // The track's previous note ends where this one begins — the sampler's own cut behaviour, and
        // the ONLY thing that ends a note when midiLen is 0 (LGPT's gate-to-next).
        end_note(ev.track, ev.frame);

        if (project_->midiSendProgramChange) send_program(ev.frame, ch, ins);
        send_cc_defaults(ev.frame, ch, ins);

        // PAN rides CC 10, and only when it moves: the value is per-note (a PAN FX bakes into the
        // record, IB-12) but re-sending an unchanged controller before every note is bytes spent on a
        // 31250-baud wire for nothing.
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
        // ⚠️ The TEMPO comes from the live project, not from `on_play`'s argument. The render path
        // passes the fallback 120 there rather than the project's tempo — an intentional quirk the
        // scheduler's goldens enshrine (event.h) — and a LEN gate that silently changed length
        // depending on whether the song was played or rendered would be a bug nobody could see.
        const int tempo = project_->tempo > 0 ? project_->tempo : tempo_;
        a.offFrame = ins.midiLen > 0
                         ? ev.frame + ins.midiLen * frames_per_tic(frames_per_step(tempo, sampleRate_))
                         : INT64_MAX;
        channelUsed_[ch] = true;
    }

    /**
     * End the track's active note at `frame` — or at its own LEN gate, whichever comes FIRST.
     *
     * ⚠️ **The `min` is the whole of LEN, and leaving it out killed the feature outright.** Every
     * event on the bus arrives at LOOKAHEAD time: by the time the clock reaches a note's gate, the
     * NEXT note has usually already been consumed, and a blind `end_note(nextFrame)` had therefore
     * cancelled the gate before it could fire. The gate only ever closed on the last note of a phrase
     * — the one case with no follower — and the byte stream looked identical either way, because a
     * cut and a gate emit the same three bytes. Only their FRAME differs, which is why ptmidi has to
     * move the clock by hand to see it at all.
     *
     * A KIL arriving before the gate still wins, which is right: `min` cuts short, never extends.
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
     * ⚠️ The three transpose fields are SEPARATE on the bus and FOLDED here (event.h says so): the
     * sampler needs chain transpose alone to pick a slice, the wire has one note number and no such
     * concept. Clamped, because a top-octave authored note (B-9 = 131) is legal in a phrase and is
     * not legal MIDI.
     */
    static int midi_note(const NoteOnPayload& n) {
        const int v = static_cast<int>(n.note) + n.transpose + n.pit + n.arp;
        return v < 0 ? 0 : (v > 127 ? 127 : v);
    }

    /**
     * Velocity, in the one place it is derived.
     *
     * `velocity` is the phrase's V column and is authoritative when present. −1 is the schema's
     * "legacy derive" (retrig and arpeggio notes, IB-19) and the only thing left to derive from is
     * `velGain`, which the scheduler built as (V/127)² — so the square root recovers the byte.
     *
     * Then `volGain` — the instrument's own VOL cell, or a Vxx override — scales it, which is how an
     * external instrument gets a working volume control at all: there is no gain stage on our side of
     * the cable. LGPT does exactly this.
     *
     * Clamped to ≥1: velocity 0 IS a note-off in MIDI, so a quiet note must never become a silent
     * one that also cancels itself.
     */
    static int midi_velocity(const NoteOnPayload& n) {
        float unit;
        if (n.velocity >= 0) {
            unit = static_cast<float>(n.velocity) / 127.0f;
        } else {
            const float g = f32_from_bits(n.velGainBits);
            unit = g > 0.0f ? std::sqrt(g) : 0.0f;
        }
        unit *= f32_from_bits(n.volGainBits);
        const int v = to7bit(unit);
        return v < 1 ? 1 : v;
    }

    // ── the patch bytes that ride a note-on ──────────────────────────────────────────────────────

    /**
     * Bank + program, when they are set and when the channel is not already on them.
     *
     * M8 sends these with every note-on. We send them when they CHANGE, which is the same thing the
     * device sees and ~5 bytes per note less on the wire — and the `forget_channel_state()` on every
     * transport start is what makes it safe: the first note of a take always states its patch, so a
     * knob turned on the gear between takes is corrected.
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
     * The instrument's CC slots, re-sent with EVERY note-on — deliberately NOT de-duplicated the way
     * bank/program is.
     *
     * These are the patch's STARTING values, and a `CCA` phrase command (phase D) or a knob on the
     * gear moves them away between notes. De-duplicating would mean the next note inherits whatever
     * the last one left behind, which is the opposite of what a default is for. M8 does the same.
     */
    void send_cc_defaults(int64_t frame, uint8_t ch, const Instrument& ins) {
        for (const MidiCcSlot& s : ins.midiCC) {
            if (s.cc < 0 || s.value < 0) continue;
            send_at(frame, MIDI_CC | ch, static_cast<uint8_t>(s.cc & 0x7F),
                    static_cast<uint8_t>(s.value & 0x7F));
        }
    }

    /** A bus CC (event.h's four ids today, more in phase D) onto the wire. */
    void cc_event(const Event& ev, const Instrument& ins) {
        const uint8_t ch = chan(ins);
        const int v7 = to7bit(f32_from_bits(ev.cc.valueBits));
        send_at(ev.frame, MIDI_CC | ch, static_cast<uint8_t>(ev.cc.param & 0x7F), static_cast<uint8_t>(v7));
        if (ev.cc.param == CC_PAN) lastPan_[ch] = v7;   // keep the note-on de-dup honest
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
        // Sorted insert from the back: the scheduler emits forward in time, so the common case is one
        // comparison. `<=` on the key keeps equal-frame messages in arrival order (upper_bound).
        auto pos = std::upper_bound(pending_.begin(), pending_.end(), m,
                                    [](const MidiMessage& a, const MidiMessage& b) {
                                        return a.frame != b.frame ? a.frame < b.frame : a.seq < b.seq;
                                    });
        pending_.insert(pos, m);
    }

    /** Bypass the queue — panic only. Nothing else may skip the clock. */
    void send_now(uint8_t status, uint8_t d1, uint8_t d2) {
        MidiMessage m;
        m.bytes[0] = status; m.bytes[1] = d1; m.bytes[2] = d2;
        m.len = 3;
        emit(m);
    }

    void emit(const MidiMessage& m) {
        if (out_ && out_->is_open()) out_->send(m.bytes, m.len);
    }

    void forget_channel_state() {
        for (int c = 0; c < 16; ++c) {
            lastBank_[c] = -2;      // -2, not -1: -1 is a REAL value ("send nothing")
            lastProgram_[c] = -2;
            lastPan_[c] = -1;
            channelUsed_[c] = false;
        }
    }

    const Project* project_ = nullptr;
    IMidiOut*      out_     = nullptr;

    TrackInstruments tracks_;
    ActiveNote       active_[TrackInstruments::LANES];

    std::vector<MidiMessage> pending_;
    uint32_t seq_ = 0;

    int tempo_ = 128, sampleRate_ = 44100, offsetMs_ = 0;

    int  lastBank_[16] = {0}, lastProgram_[16] = {0}, lastPan_[16] = {0};
    bool channelUsed_[16] = {false};
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_OUT_H
