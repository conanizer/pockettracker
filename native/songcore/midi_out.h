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
//   • **the route to the wire.** Since phase C there are two producers — this serializer and the
//     `MidiClock` below — and they share `emit`, the OFFSET, the port and the send observer. The clock
//     hands its bytes to a sink rather than holding a port of its own, for exactly that reason.
//
// ── TIMING, AND WHO CALLS `pump` (phase B3) ──────────────────────────────────────────────────────
//
// Events arrive at LOOKAHEAD time — the scheduler runs two phrases (~4 s) ahead — so they are queued
// against their target frame and released by `pump(now)`. `pump` is called from wherever the frame
// clock is read: until B3 that was the host's 60 Hz poll, which quantised every send to ~16 ms. Since
// B3 it is a dedicated sender thread (`shell/midi-sender.{h,cpp}`) running at ~1 kHz against
// `FrameEstimator` (songcore/frame_estimator.h), and `SongcoreHost::poll` pumps only when no such
// thread has claimed the job — one owner of "who releases the queue", the same rule B4.3 applied to
// "who opens the port".
//
// ⚠️ **THE PLAN SAID THIS WOULD CHANGE THIS FILE "NOT AT ALL". THAT WAS A HYPOTHESIS AND IT WAS
// WRONG** — a mechanism in a ratified plan is a hypothesis (the guardrails' own rule). `pump` taking
// its `now` as an argument is indeed all the SEAM needed, but a second thread calling it made this
// class's state shared, and shared state needs a lock. Hence `mu_` below. Everything else about the
// serializer, the queue and the note lifecycle is genuinely unchanged.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "event.h"
#include "midi_clock.h"    // phase C: the 24 PPQN grid and the transport bytes
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

/** No bend — the 14-bit value a channel rests at, and what a panic restores it to. */
constexpr int MIDI_BEND_CENTRE = 0x2000;

/**
 * One message, stamped with the frame it is due on.
 *
 * `seq` is the arrival counter and exists only to make the queue's order TOTAL: two messages due on
 * the same frame must leave in the order they were produced (bank → program → CC → note-on is
 * meaningless otherwise), and `std::sort` on frame alone would be free to swap them.
 *
 * `frame == UNSCHEDULED` is the panic path — a message that bypassed the clock entirely. Only
 * `send_now` produces it, and the only thing that reads it is an `IMidiSendObserver` measuring
 * lateness, which must not fit a straight line through a message that was never due at a time.
 */
struct MidiMessage {
    static constexpr int64_t UNSCHEDULED = INT64_MIN;

    int64_t  frame = 0;
    uint32_t seq   = 0;
    uint8_t  bytes[3] = {0, 0, 0};
    uint8_t  len   = 0;
};

/**
 * The instrument seam for phase B3: told about every message the moment it is released.
 *
 * ⚠️ **THIS IS HOW B3 IS MEASURED AT ALL, and nothing else in the tree can do it.** `pump`'s whole
 * job is to release a message CLOSE TO ITS DUE FRAME, and every existing instrument is blind to that:
 * ptmidi drives a synthetic clock (so it measures the arithmetic, never the delivery), the trace prints
 * bytes with no time, and the byte stream is byte-identical whether a note left 1 ms or 40 ms late.
 * An observer that stamps its OWN wall clock beside `dueFrame` can fit the two against each other and
 * report the residual in milliseconds — see `shell/midi-sender.h`.
 *
 * ⚠️ Called with the lock held and from whichever thread released the message (the sender thread for
 * anything queued, the frame loop for a panic), so an implementation must be quick and must not call
 * back into `ExternalConsumer`.
 *
 * ⚠️ Called EVEN WHEN NO PORT IS OPEN — `sent` says which. That is deliberate and it is what makes
 * the measurement runnable with no cable, no synth and no hardware: B3 is about WHEN the queue
 * releases, which is upstream of the port. It also keeps "released but not sent" distinguishable from
 * "never released", two states that a port-side trace collapses into silence.
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

    // ── ⚠️ THREADING (phase B3) ──────────────────────────────────────────────────────────────────
    //
    // TWO threads reach this object and the split is not symmetrical:
    //
    //   • the FRAME LOOP produces (`consume` off the router, `on_play`, the host's preview) and also
    //     PANICS (transport stop, a port swap, teardown);
    //   • the SENDER THREAD only ever calls `pump`.
    //
    // So every public method takes `mu_` and every private helper assumes it is already held — which
    // is why `on_play` and `set_out` call `panic_locked()` and not `panic()`. A `std::mutex` is not
    // recursive; the two-layer split is what keeps that from being a deadlock waiting for the next
    // person to add a call. (The guardrails' rule: if correctness rests on every future site
    // remembering something, write it once BELOW the sites.)
    //
    // ⚠️ **The port write happens under the lock, and that is on purpose.** `IMidiOut::send` is not
    // required to be thread-safe and ALSA rawmidi's write plainly is not — serialising every message
    // through this one lock is what makes all three backends safe without each of them needing a lock
    // of its own. The price is that a blocking write stalls the producer: bounded by a 4 KB rawmidi
    // buffer against a few hundred bytes per second (see shell/midi-out-alsa.cpp), and the alternative
    // — dropping a note-off — is the failure this whole file exists to prevent.
    //
    // Uncontended cost: one atomic exchange per event, on a path that already walks a sorted insert.

    /**
     * Attach (or detach, with nullptr) the port.
     *
     * ⚠️ Panics FIRST. Swapping the cable out from under sounding notes is the one moment where the
     * note-offs we owe can no longer be delivered — after the swap the new port has never heard the
     * note-ons, and the old device holds them forever.
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
     * The bus's "which instrument is track N playing?" state, READ-ONLY — MIDI in's router asks it
     * (midi_in.h, phase E) rather than keeping a third opinion about track ownership.
     *
     * ⚠️ **NOT LOCKED, AND THAT IS DELIBERATE RATHER THAN AN OVERSIGHT.** `tracks_` is written in
     * `consume` and read here, and both of those run on the frame-loop thread; the SENDER thread only
     * ever calls `pump`, which does not touch it. Returning a locked copy would say this is shared
     * state when it is not, and taking `mu_` here would deadlock the moment a caller holds it. ⚠️ A
     * future second writer of `tracks_` invalidates this sentence, which is why it is written down.
     */
    const TrackInstruments& track_instruments() const { return tracks_; }

    /** The B3 instrument, or nullptr for none. Set once at boot, before the sender thread starts. */
    void set_send_observer(IMidiSendObserver* obs) {
        std::lock_guard<std::mutex> lk(mu_);
        observer_ = obs;
    }

    /**
     * SYNC OUT — the 24 PPQN clock and the transport bytes (phase C, midi_clock.h).
     *
     * ⚠️ Turning it OFF mid-take sends the Stop the device is owed. A synth left on external sync with
     * the clock silently withdrawn waits forever for a tick that is never coming, which looks exactly
     * like the app having crashed. `MidiClock::set_enabled` returns whether that Stop is owed, derived
     * from the clock's own state rather than from this call site knowing.
     *
     * ⚠️ Turning it ON mid-take takes effect at the NEXT transport start, and that is a decision rather
     * than a gap. The grid's epoch is the take's start frame — that is what puts tick 6k on step k —
     * and a clock armed halfway through a take would have to invent an epoch, landing every tick
     * between the steps and reporting a song position it never measured. There is nothing musical to
     * salvage there; the honest behaviour is to begin at a beginning.
     */
    void set_sync_out(bool on) {
        std::lock_guard<std::mutex> lk(mu_);
        if (clock_.set_enabled(on)) send_now_1(MIDI_RT_STOP);
    }
    bool sync_out() const {
        std::lock_guard<std::mutex> lk(mu_);
        return clock_.enabled();
    }

    /**
     * The user's alignment between the audio the speakers produce and the notes the cable carries
     * (plan §4.3). Positive = MIDI later. Milliseconds, applied as frames at send time.
     */
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
     * ⚠️ **`on_play` IS the transport starting — unlike `on_stop` below, and the asymmetry is real.**
     * `t_play` is emitted once, by `playSong`/`playChain`/`playPhrase` (and by the render pass, which
     * never reaches this consumer — the host detaches the cable for a render). `t_stop` is emitted at
     * the end of every scheduling pass. Verified in scheduler.h, not assumed from the pair of names.
     *
     * ⚠️ **The `panic()` is what makes `tracks_.reset()` safe, and it was NOT safe before B5.**
     * Resetting the lane→instrument map while `active_` still holds notes strands them: no later event
     * on that lane resolves to an external instrument any more, so `consume` returns at the gate and
     * the note-off we owe can never be delivered. On tracks 0-7 the next take's first note-on hid it
     * (`note_on` ends the track's previous note first); the PREVIEW lane has no next note-on, so an
     * audition ringing when the transport started would have hung. Panicking here is also just what a
     * transport start means: nothing may be sounding from before it, and the queue holds messages for
     * a take that no longer exists.
     */
    void on_play(const std::string& kind, const std::string& detail, int64_t startFrame,
                 int tempo, int sample_rate) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (tempo > 0) tempo_ = tempo;
        if (sample_rate > 0) sampleRate_ = sample_rate;
        // A fresh transport is a fresh device state: the bank/program/pan we believe each channel is
        // on was true of the last take, and the user may have turned knobs on the gear since.
        // (`panic_locked` ends every sounding note, drops the queue, sends the Stop a running clock is
        // owed, and calls forget_channel_state().)
        panic_locked();
        tracks_.reset();

        // ── phase C: arm the clock, and say WHERE the take starts ────────────────────────────────
        //
        // ⚠️ **THE START ROW IS READ OUT OF `detail`, AND THAT IS THE DELIBERATE CHOICE.** The obvious
        // alternative — `SongcoreHost::play_song` telling this consumer the row before it starts the
        // sequencer — needs three call sites (song, chain, phrase) to each remember to set or clear it,
        // which is the shape the guardrails say is already broken. `kind`/`detail` are data that
        // ALREADY flows here on exactly the event that matters, and they cannot silently change out
        // from under this: all 36 trace goldens byte-compare those two strings.
        //
        // Only SONG carries a row. CHAIN and PHRASE are auditions of one object rather than positions
        // in a song, so they start the device at 0. RENDER never reaches this consumer at all — the
        // host detaches the cable for a render (host.h `prepare_render`) — and its detail is `rows=`,
        // which is why the lookup asks for `row=` and gets no match if one ever did arrive.
        const int startRow = kind == "SONG" ? hex_after(detail, "row=") : 0;
        const int spp      = (startRow > 0 && project_) ? nominal_spp_beats(*project_, startRow) : 0;
        clock_.start(startFrame, frames_per_quarter(), spp);
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
        std::lock_guard<std::mutex> lk(mu_);
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
            case EV_PROGRAM: program_event(ev, ins); break;
            case EV_PITCH_BEND: pitch_bend_event(ev, ins); break;
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
     * Late, never early: a message is sent on the first pump at or after its due frame. So the CALLER'S
     * CADENCE IS THE PRECISION — 0–16 ms of lateness from the 60 Hz frame loop that used to make this
     * call, sub-millisecond from B3's sender thread. Nothing in here changed to get that; who calls it,
     * and with what `now`, is the whole of the difference (shell/midi-sender.h).
     */
    void pump(int64_t nowFrame) {
        std::lock_guard<std::mutex> lk(mu_);
        // ONE timeline for both halves: `due` is "now" expressed in EVENT frames, so a LEN gate and a
        // queued message are released by the same clock. Checking the gate against the raw frame and
        // the queue against the offset one would make OFFSET silently mean two different things.
        const int64_t due = nowFrame - offset_frames();
        // ── The clock, before anything else this pump releases (phase C) ─────────────────────────
        //
        // First because the transport bytes ride tick 0's due frame and a Start must precede the take
        // it starts. The ticks themselves are system real-time, which the MIDI spec lets a sender
        // interleave anywhere — so "clock before notes at the same frame" costs nothing and buys the
        // one ordering that does matter.
        //
        // ⚠️ It rides the OFFSET-corrected `due`, with the notes. Shifting notes and leaving the clock
        // where it was would make the OFFSET row mean two different things on one cable.
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
     * All notes off, right now, on every channel we have touched — and the pending queue dropped.
     *
     * ⚠️ Both halves are load-bearing. The per-note offs are what a device needs to stop the notes it
     * knows about; the CC 123 is the backstop for the ones our bookkeeping has lost (a hot-plug, a
     * crash mid-take, a device that missed a byte). And the queue must go: it holds note-ons for a
     * transport that no longer exists, and releasing them after a stop is a stuck note by definition.
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
     * Is there anything this consumer owes SOON? The sender thread's busy/idle test.
     *
     * ⚠️⚠️ **IT USED TO BE `pending_count() > 0`, AND PHASE C IS EXACTLY THE LAYER THAT INVALIDATED
     * THAT — the recurring shape, again: an assumption true when it was made, broken by the thing built
     * on top of it, in a channel nothing was pointed at.** B3 wrote the idle test when the only work
     * this object had was messages sitting in `pending_`. The clock is a SECOND PRODUCER that queues
     * nothing at all — its ticks are generated inside `pump` from the frame it is handed — so a song of
     * pure sync out (no EXTERNAL instrument anywhere) has an empty queue at every instant, and the
     * thread would have idled at 4 ms. On a 19.5 ms tick that is 20% of a beat of jitter, silently, on
     * the one signal phase C exists to deliver.
     *
     * ⭐ So the predicate is DERIVED FROM THE DEADLINES rather than from a flag somebody must remember
     * to add: a running clock owes a tick, a non-empty queue owes a message. (The LEN gates are
     * deliberately NOT in here. A gate is bounded by 4 ms of idle tick, which nobody can hear, and B3
     * decided that on purpose — see shell/midi-sender.cpp.)
     */
    bool needs_fast_pump() const {
        std::lock_guard<std::mutex> lk(mu_);
        return !pending_.empty() || clock_.running();
    }

    /** The clock's own window, for the tools and the shell's instrument. Never for a decision. */
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
        // ⚠️ THE STOP GOES FIRST, and it is not cosmetic: a device on external sync stops its own
        // sequencer on 0xFC, and everything below is us ending OUR notes. Both belong to the same
        // instant, so the transport event leads and the housekeeping follows.
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
        // A bent channel is silent and still wrong — see pitch_bend_event. Independent of
        // channelUsed_: MPB on a track that never sounded a note still bent the device.
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

    /**
     * A bus CC onto the wire — a literal controller number, or one of the four SYMBOLIC slot ids.
     *
     * ⚠️ A slot id (event.h `CC_SLOT_A`, 128-131) is NOT a controller number and must never reach the
     * `& 0x7F` below: masked, 128 is CC 0 — bank select — and a `CCA` on an instrument whose slot A is
     * unassigned would re-bank the device instead of doing nothing. `resolve_cc_param` (model.h) is
     * where that translation lives, shared with the engine consumer so the two cannot disagree;
     * resolving it HERE rather than in the scheduler is the point of the id (see event.h).
     */
    void cc_event(const Event& ev, const Instrument& ins) {
        const int param = resolve_cc_param(ins, ev.cc.param);
        if (param < 0) return;    // an unassigned slot — the command has no controller to move
        const uint8_t ch = chan(ins);
        const int v7 = to7bit(f32_from_bits(ev.cc.valueBits));
        send_at(ev.frame, MIDI_CC | ch, static_cast<uint8_t>(param & 0x7F), static_cast<uint8_t>(v7));
        if (param == CC_PAN) lastPan_[ch] = v7;   // keep the note-on de-dup honest
    }

    /**
     * MPG — an explicit program change on the track's channel (phase D).
     *
     * ⚠️ **It must move `lastProgram_`, and that line is the whole correctness of the feature.**
     * `send_program` skips when it believes the channel is already on the instrument's patch; after an
     * MPG the channel is on something else entirely, and a de-dup that had not been told would leave
     * the instrument's next note-on silently playing the MPG's program forever. Same rule, same
     * reason, as `cc_event` writing `lastPan_` — a cache of what the DEVICE believes, kept honest by
     * everything that changes what the device believes.
     *
     * The bank is cleared to the "unknown" sentinel rather than to the instrument's: a program change
     * selects within whatever bank the device is on, and claiming to know which one would suppress the
     * bank bytes the next note-on may genuinely owe.
     */
    void program_event(const Event& ev, const Instrument& ins) {
        const uint8_t ch = chan(ins);
        const int prog = ev.program.program & 0x7F;
        send_at(ev.frame, MIDI_PROGRAM | ch, static_cast<uint8_t>(prog));
        lastProgram_[ch] = prog;
        lastBank_[ch] = -2;
    }

    /**
     * MPB — an absolute 14-bit pitch bend on the track's channel (phase D).
     *
     * ⚠️ **A BEND IS A SECOND KIND OF HANGING STATE, AND THE PANIC DID NOT CLEAR IT.** Everything this
     * file does to guarantee nothing is left sounding is about NOTES; a channel left bent a whole tone
     * up is silent and still wrong — every later note on that device, from this app or the next one,
     * plays detuned until someone power-cycles it. So a bent channel is TRACKED exactly as an active
     * note is, and `panic_locked` returns it to centre. Tracked rather than "always centre all 16":
     * bending a channel we never touched is us breaking somebody else's state.
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
        m.frame = MidiMessage::UNSCHEDULED;   // never had a due time; see IMidiSendObserver
        m.bytes[0] = status; m.bytes[1] = d1; m.bytes[2] = d2;
        m.len = 3;
        emit(m, MidiMessage::UNSCHEDULED);
    }

    /** The one-byte form of the same thing — the system real-time Stop, and only that. */
    void send_now_1(uint8_t status) {
        MidiMessage m;
        m.frame = MidiMessage::UNSCHEDULED;
        m.bytes[0] = status;
        m.len = 1;
        emit(m, MidiMessage::UNSCHEDULED);
    }

    /**
     * A message the CLOCK produced: it already carries its due frame and is already at or behind
     * `now`, so it goes straight out rather than through `pending_`.
     *
     * ⚠️ It still carries a real `frame`, which is what makes clock lateness MEASURABLE — the shell's
     * `MidiJitterRecorder` fits wall time against due frame and can only do that for a message that
     * had a due time. A clock stamped UNSCHEDULED would be excluded from the fit, and phase C's whole
     * claim is about milliseconds.
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
     * Frames per quarter note at the LIVE tempo — the clock's period source, read fresh on every pump.
     *
     * ⚠️ The tempo is the project's, not `on_play`'s argument, for the same reason `note_on`'s LEN gate
     * uses the project's: the render path passes the fallback there and TEMPO is editable while
     * playing. A quarter is four phrase steps (TICS_PER_STEP = 12, so 48 tics — plan §10-C), and
     * multiplying the already-truncated `frames_per_step` is deliberate: it keeps the clock on the same
     * rounded grid the scheduler puts notes on rather than on a more accurate one they would drift from.
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
        // ⚠️ OUTSIDE the `sent` test on purpose — the B3 measurement must work with no cable attached,
        // and "released but there was no port" is a different fact from "never released". See
        // IMidiSendObserver.
        if (observer_) observer_->on_released(m, nowFrame, sent);
    }

    void forget_channel_state() {
        for (int c = 0; c < 16; ++c) {
            lastBank_[c] = -2;      // -2, not -1: -1 is a REAL value ("send nothing")
            lastProgram_[c] = -2;
            lastPan_[c] = -1;
            channelUsed_[c] = false;
            bent_[c] = false;
        }
    }

    // `mutable` so the const readers (`out()`, `offset_ms()`, `pending_count()`) can take it — a read
    // of a value another thread writes is not const in any sense that matters.
    mutable std::mutex mu_;

    const Project*     project_  = nullptr;
    IMidiOut*          out_      = nullptr;
    IMidiSendObserver* observer_ = nullptr;

    TrackInstruments tracks_;
    ActiveNote       active_[TrackInstruments::LANES];
    MidiClock        clock_;   // phase C — no lock of its own; `mu_` above is the one that guards it

    std::vector<MidiMessage> pending_;
    uint32_t seq_ = 0;

    int tempo_ = 128, sampleRate_ = 44100, offsetMs_ = 0;

    int  lastBank_[16] = {0}, lastProgram_[16] = {0}, lastPan_[16] = {0};
    bool channelUsed_[16] = {false};
    bool bent_[16] = {false};   // channels an MPB has moved off centre (phase D)
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_OUT_H
