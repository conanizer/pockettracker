#ifndef POCKETTRACKER_SONGCORE_MIDI_IN_H
#define POCKETTRACKER_SONGCORE_MIDI_IN_H

// ─── MIDI IN — bytes from a cable become bus events (MIDI plan phase E, §5) ──────────────────────
//
// The mirror image of midi_out.h, and deliberately shaped like it: `IMidiIn` is the platform seam,
// everything above it is platform-free, and the one routing question ("whose note is this?") is
// answered by ONE rule that every consumer asks rather than by each consumer's private test.
//
// Three objects, and the split is the point — each is the answer to a different kind of question:
//
//   • `MidiInQueue`  — WHO OWNS THE THREAD. A backend receives bytes on a thread it does not choose
//     (winmm calls back on its own; ALSA blocks in `read`; Android delivers on a binder thread), so
//     the bytes are parked in one lock-guarded ring and the app drains them where it likes. Written
//     ONCE, above the seam, for the reason midi-out-base.h states: three backends each carrying their
//     own ring is three rings that diverge in one of the copies.
//   • `MidiParser`   — THE PROTOCOL, and nothing else. Running status, real-time bytes arriving in the
//     middle of another message, SysEx, orphan data bytes. It reports the wire FAITHFULLY and makes no
//     policy decisions at all — including the note-on-velocity-0 convention, which is a `MidiInMessage`
//     predicate below rather than a rewrite here, so that every future reader of a message gets the
//     same answer without remembering to.
//   • `MidiInputRouter` — THE POLICY. Which track does channel 5 drive, which instrument is that
//     track's, and what does a bus record for it look like. This is where `Project::midiInputChannels`
//     — round-tripped since B1 and, until now, read by NOTHING — finally has a consumer.
//
// ⚠️ **NOTHING HERE TOUCHES A CLOCK OR A PORT.** `route` takes the frame as an argument, exactly as
// `ExternalConsumer::pump` and `FrameEstimator::estimate` take theirs, and it returns records into a
// caller's array rather than calling anything. That is what makes the whole of phase E's protocol half
// testable in a host tool with no device, no cable and no wall clock (tools/ptmidiin).
//
// ── WHAT IS DELIBERATELY *NOT* HERE ──────────────────────────────────────────────────────────────
//
//   • **SYNC IN** (slaving the transport to an external clock) is §9-deferred. The parser therefore
//     REPORTS real-time bytes (0xF8-0xFF) as one-byte messages — the protocol is the protocol — and the
//     router drops them and COUNTS the drop. A parser that swallowed them would have to be reopened to
//     add sync; a router that dropped them silently could not be told from one that never ran.
//   • **the §4.1 note-off rule** (ADSR/TRIG release, one-shot ignore, looping soft-kill) — the RULE
//     itself, which is an engine decision and lives in `SamplerVoice::keyRelease`. What this file does
//     since E4 is name it: a key release emits `NOTE_OFF_KEY`, which is a different thing from the
//     `NOTE_OFF_RELEASE` a KIL emits, and the engine consumer translates the two separately.
//   • **recording into phrases** and the MIDI-learn map — §9, both.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "event.h"
#include "model.h"
#include "router.h"   // TrackInstruments — the shared "whose track is this?" rule

namespace songcore {

// ─── The platform seam ──────────────────────────────────────────────────────────────────────────

/**
 * Where a backend puts the bytes it just received.
 *
 * ⚠️ **CALLED FROM A THREAD YOU DO NOT OWN, and on every platform it is a different one** — a winmm
 * callback, an ALSA reader thread, an Android binder thread. An implementation must be quick, must not
 * block, and must not call back into the backend. `MidiInQueue` below is the one implementation the
 * app uses; a test is the only other caller.
 */
struct IMidiInSink {
    virtual ~IMidiInSink() = default;
    virtual void on_bytes(const uint8_t* data, int len) = 0;
};

/**
 * A MIDI input port. Implemented once per platform; nothing above this interface is per-platform.
 *
 * Deliberately the same five list/open/close methods as `IMidiOut`, plus `set_sink` — the direction of
 * travel is the only difference, and the MIDI screen's INPUT row wants to ask a port exactly what the
 * OUTPUT row asks its own.
 *
 * ⚠️ `set_sink(nullptr)` must be safe at any moment and must be honoured before `close()` returns, or a
 * backend thread outlives the object it is writing into.
 */
struct IMidiIn {
    virtual ~IMidiIn() = default;

    /** How many input devices exist right now. Re-enumerated on demand; hot-plug changes it. */
    virtual int device_count() = 0;
    /** Display name of device `index`, for the MIDI screen's INPUT row. */
    virtual std::string device_name(int index) = 0;

    virtual bool open(int index) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    /** Where received bytes go. Set BEFORE `open`, cleared before the sink dies. */
    virtual void set_sink(IMidiInSink* sink) = 0;
};

// ─── The queue — the one thread boundary ────────────────────────────────────────────────────────

/**
 * A bounded byte ring between the backend's thread and the app's.
 *
 * Bytes and not messages, deliberately: the backend has no parser and must not need one (ALSA hands
 * over whatever a `read` returned, which can split a message down the middle), and a byte ring
 * RESYNCS by construction — the parser ignores data bytes until the next status byte, so even a
 * mangled stream costs at most one message.
 *
 * A plain mutex, not a lock-free ring: the traffic is a few KB per second at the very worst (a
 * controller sweeping every knob), the critical section is a memcpy, and a lock-free ring here would be
 * a subtle thing written once and never exercised hard enough to find its bug.
 *
 * ⚠️ **OVERFLOW IS COUNTED, NEVER SILENT** — the same rule as `MidiClock::dropped_ticks()`: a component
 * that quietly swallows work cannot be told from one that had nothing to do. `dropped()` is printed
 * beside the verdict wherever this is reported.
 */
class MidiInQueue : public IMidiInSink {
  public:
    // 1 KB = ~341 three-byte messages. A 60 Hz drain would need 20 000 messages/second to overflow it,
    // which is about twenty times what a MIDI 1.0 cable can physically carry (31 250 baud ≈ 1 040
    // three-byte messages/second). The counter exists for the case this reasoning is wrong.
    static constexpr int CAPACITY = 1024;

    void on_bytes(const uint8_t* data, int len) override {
        if (!data || len <= 0) return;
        std::lock_guard<std::mutex> lock(mu_);
        for (int i = 0; i < len; ++i) {
            if (count_ >= CAPACITY) {
                // Drop the NEWEST, keeping every complete message already received. Dropping the
                // oldest instead would throw away note-ONs whose note-offs are still coming — the one
                // failure mode of this whole file that costs a stuck note.
                dropped_ += static_cast<uint64_t>(len - i);
                return;
            }
            buf_[(head_ + count_) % CAPACITY] = data[i];
            ++count_;
        }
    }

    /** Move up to `max` bytes into `out`. Returns how many. Called from the app's thread. */
    int drain(uint8_t* out, int max) {
        if (!out || max <= 0) return 0;
        std::lock_guard<std::mutex> lock(mu_);
        const int n = count_ < max ? count_ : max;
        for (int i = 0; i < n; ++i) out[i] = buf_[(head_ + i) % CAPACITY];
        head_ = (head_ + n) % CAPACITY;
        count_ -= n;
        return n;
    }

    /** Bytes lost to a full ring, ever. Nonzero means the drain is not keeping up. */
    uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mu_);
        return dropped_;
    }

    int pending() const {
        std::lock_guard<std::mutex> lock(mu_);
        return count_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        head_ = count_ = 0;
    }

  private:
    mutable std::mutex mu_;
    uint8_t  buf_[CAPACITY] = {0};
    int      head_ = 0, count_ = 0;
    uint64_t dropped_ = 0;
};

// ─── The protocol's one arithmetic fact ─────────────────────────────────────────────────────────

/**
 * How many bytes a message with this status byte occupies on the wire — 1, 2 or 3.
 *
 * ⚠️ **ONE COPY, AND IT HAS TWO READERS A LONG WAY APART.** `MidiParser` below needs it to know how
 * many data bytes to collect. But a BACKEND needs it too, and that was not obvious until E2: winmm
 * hands over a PACKED short message (`MIM_DATA` is a DWORD with the status in the low byte) and does
 * not say how much of it is real. A backend that assumes three turns one program change into TWO — the
 * pad byte lands as a data byte under running status and is a second, silent PC to program 0.
 *
 * ⚠️ A real-time byte is always 1 and never interrupts anything (see `feed`). 0xF4/0xF5 are undefined
 * and 0xF6 is tune request: one byte each.
 */
inline int midi_message_length(uint8_t status) {
    if (status >= 0xF8) return 1;
    if (status < 0xF0) {
        const uint8_t hi = status & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;   // program change / channel pressure carry one
    }
    switch (status) {
        case 0xF1: return 2;   // MTC quarter frame
        case 0xF2: return 3;   // song position pointer
        case 0xF3: return 2;   // song select
        default:   return 1;   // 0xF4 0xF5 undefined, 0xF6 tune request
    }
}

// ─── The parser ─────────────────────────────────────────────────────────────────────────────────

/**
 * One complete MIDI 1.0 message off the wire.
 *
 * `status` is the STATUS NIBBLE for a channel message (0x80-0xE0, channel split out) and the whole
 * byte for anything system (0xF1-0xFF). That split is what lets a consumer switch on `status` alone.
 */
struct MidiInMessage {
    uint8_t status  = 0;   // 0x80-0xE0 channel (nibble) | 0xF1-0xFF system (whole byte)
    uint8_t channel = 0;   // 0-15; meaningless for system messages
    uint8_t data1   = 0;
    uint8_t data2   = 0;
    uint8_t len     = 0;   // 1-3, as it appeared on the wire (running status excluded)

    bool is_channel()  const { return status >= 0x80 && status <= 0xE0; }
    bool is_realtime() const { return status >= 0xF8; }

    /**
     * ⚠️ **THE NOTE-ON/NOTE-OFF RULE, AND IT LIVES HERE SO IT IS ASKED ONCE.**
     *
     * A note-on with velocity 0 IS a note-off — it is how running status lets a keyboard send a whole
     * phrase as one status byte followed by pairs, and essentially every controller does it. A consumer
     * that switches on the status byte alone therefore hears a note-on it never answers, and an
     * unanswered note-on on an external synth sounds until the power is cut.
     *
     * The parser does NOT rewrite the message, because the wire said what it said and a parser that
     * edits its input cannot be checked against a byte stream. The rule is a predicate instead — one
     * copy, below every site, which is the guardrails' own answer to "every future caller must
     * remember to".
     */
    bool is_note_on()  const { return status == EV_NOTE_ON  && data2 > 0; }
    bool is_note_off() const { return status == EV_NOTE_OFF || (status == EV_NOTE_ON && data2 == 0); }
};

/**
 * A streaming MIDI 1.0 parser: feed it bytes, it tells you when a message is complete.
 *
 * Zero allocation, zero policy, and it owns nothing but four bytes of state. Everything it handles is
 * something a real cable does and a naive `if (b & 0x80) ...` does not:
 *
 *   • **running status** — a status byte holds until the next one, so `90 3C 40 3E 40 40 40` is three
 *     note-ons. Without it a keyboard's fast passages arrive as orphan data bytes.
 *   • **real-time bytes interleaved MID-MESSAGE.** 0xF8-0xFF may legally appear between the status byte
 *     and its data, or between two data bytes, and they must change NOTHING: not the running status,
 *     not the half-assembled message. A parser that resets on any status byte ≥ 0x80 eats the note it
 *     was in the middle of — and only while a clock is running, which is exactly when nobody is
 *     watching for it.
 *   • **System Common cancels running status** (MIDI 1.0 spec). 0xF1-0xF6 are one-shot; a data byte
 *     after one is an orphan, not a continuation of the channel message before it.
 *   • **SysEx is skipped** to its 0xF7, and cancels running status on the way in.
 *   • **an orphan data byte is dropped and counted.** It means the stream was joined mid-message (a
 *     cable plugged in while a controller was mid-sweep, or a ring overflow) and resync is the correct
 *     behaviour, but a stream that is *all* orphans is a bug and the count is the only thing that says
 *     so.
 */
class MidiParser {
  public:
    /** Feed one byte. Returns true when `message()` holds a newly completed message. */
    bool feed(uint8_t b) {
        // ── real time: complete in one byte, and interrupts NOTHING ──
        if (b >= 0xF8) {
            msg_ = MidiInMessage{};
            msg_.status = b;
            msg_.len    = 1;
            return true;                      // status_, pending_, running status: all untouched
        }

        if (b >= 0x80) {                      // a status byte
            if (b == 0xF7) { inSysex_ = false; return false; }          // end of SysEx
            pendingCount_ = 0;
            if (b == 0xF0) {                  // start of SysEx — skip its body
                inSysex_ = true;
                status_  = 0;
                return false;
            }
            inSysex_ = false;
            status_  = b;
            expect_  = data_bytes_for(b);
            if (expect_ == 0) {               // 0xF6 tune request, 0xF4/0xF5 undefined
                emit();
                status_ = 0;                  // system messages never establish running status
                return true;
            }
            return false;
        }

        // ── a data byte ──
        if (inSysex_) return false;
        if (status_ == 0) { ++orphans_; return false; }   // no running status to belong to

        pending_[pendingCount_++] = b;
        if (pendingCount_ < expect_) return false;

        emit();
        pendingCount_ = 0;
        if (status_ >= 0xF0) status_ = 0;     // System Common is one-shot; channel status runs on
        return true;
    }

    const MidiInMessage& message() const { return msg_; }

    /** Data bytes seen with no status byte to belong to. A resync, or a bug — nonzero is worth a look. */
    uint64_t orphan_bytes() const { return orphans_; }

    /** Forget everything mid-flight. Called when a port closes, so the next one starts clean. */
    void reset() {
        status_ = 0;
        expect_ = pendingCount_ = 0;
        inSysex_ = false;
        msg_ = MidiInMessage{};
    }

  private:
    // The status byte's own length, minus the status byte itself. ⚠️ Not a second copy of the table:
    // `midi_message_length` above is the one place that knows, because a backend needs the same answer
    // (see its note) and two copies of a protocol constant is how a program change becomes two.
    static int data_bytes_for(uint8_t status) { return midi_message_length(status) - 1; }

    void emit() {
        msg_ = MidiInMessage{};
        if (status_ < 0xF0) {
            msg_.status  = status_ & 0xF0;
            msg_.channel = status_ & 0x0F;
        } else {
            msg_.status = status_;
        }
        msg_.data1 = expect_ > 0 ? pending_[0] : 0;
        msg_.data2 = expect_ > 1 ? pending_[1] : 0;
        msg_.len   = static_cast<uint8_t>(1 + expect_);
    }

    MidiInMessage msg_;
    uint8_t  status_ = 0;          // the status byte in force — running status, once established
    int      expect_ = 0;          // data bytes `status_` wants
    uint8_t  pending_[2] = {0, 0};
    int      pendingCount_ = 0;
    bool     inSysex_ = false;
    uint64_t orphans_ = 0;
};

// ─── The router — channel to track, track to instrument, message to bus record ──────────────────

/**
 * Turns a parsed message into the bus records it means, for whichever tracks are listening.
 *
 * ⚠️ **THE INSTRUMENT IS NOT IN THE MAP, AND THAT IS THE RATIFIED DATA MODEL** (§7): a track's input
 * entry is a CHANNEL and nothing else, so "which instrument does this key play?" has to be answered
 * from somewhere. It is answered the way §5 words it — *the track's current instrument* — which in this
 * engine is `TrackInstruments`, the SAME object both existing consumers use to decide who owns a
 * track-scoped event. Nothing here gets a private opinion about track ownership.
 *
 * ⚠️ **With a fallback, and the fallback is what makes the feature work at all on the first try.**
 * `TrackInstruments` learns from note-ons, so on a stopped song — or a track that has not played yet —
 * it knows nothing, and a keyboard would be silent with everything correctly configured. That is the
 * failure mode §B2 wrote down the hard way: *a feature whose "not configured" state is
 * indistinguishable from its "broken" state will be reported as broken.* So the host supplies the
 * instrument the UI is currently showing — the same one the A-button preview auditions — and a live key
 * plays what you are looking at until the sequencer says otherwise.
 *
 * Fan-out is real: several tracks may name the same input channel, which is how a controller layers
 * across tracks, so one message can produce up to `POOL_TRACKS` records.
 */
class MidiInputRouter {
  public:
    // One event per track, at most: a message never produces two records for one track.
    static constexpr int MAX_EVENTS = POOL_TRACKS;

    void set_project(const Project* p) { project_ = p; }
    /** The shared "which instrument is this track playing?" state. Read-only here. */
    void set_track_instruments(const TrackInstruments* ti) { tracks_ = ti; }
    /** The instrument a track with no history plays — the UI's current one. −1 = none, so drop. */
    void set_fallback_instrument(int id) { fallback_ = id; }
    int  fallback_instrument() const { return fallback_; }

    /**
     * Route one message onto the bus at `frame`. Returns how many records were written to `out`.
     *
     * `frame` is the caller's: MIDI in is a live source with no lookahead, so the host passes the
     * transport clock plus its own lead-in, exactly as `preview_note` does.
     */
    int route(const MidiInMessage& msg, int64_t frame, Event* out, int maxOut) {
        if (!project_ || !out || maxOut <= 0) return 0;

        // Real time and System Common: the protocol has them, this phase does not. SYNC IN is
        // §9-deferred and it is the only thing that would read them.
        if (!msg.is_channel()) { ++nonChannel_; return 0; }

        const int tracks = static_cast<int>(project_->midiInputChannels.size());
        int n = 0;
        bool anyTrack = false;

        for (int t = 0; t < tracks && n < maxOut; ++t) {
            if (project_->midiInputChannels[static_cast<size_t>(t)] != static_cast<int>(msg.channel)) continue;
            anyTrack = true;

            const int instrument = input_instrument(t);
            if (instrument < 0) { ++noInstrument_; continue; }

            if (build(msg, frame, static_cast<uint8_t>(t), instrument, out[n])) {
                ++n;
                ++routed_;
            } else {
                ++unsupported_;
            }
        }

        if (!anyTrack) ++unmapped_;
        return n;
    }

    // ── counters: every path that produces no event says which one it was ────────────────────────
    // ⭐ A component whose correct behaviour is silence cannot be told from one that never ran. Four
    // separate reasons, because "nothing happened" has four completely different fixes: turn on SYNC
    // (nonChannel), map a track (unmapped), pick an instrument (noInstrument), or nothing at all
    // (unsupported — aftertouch, which this engine has no form for).
    uint64_t routed() const { return routed_; }
    uint64_t nonChannel() const { return nonChannel_; }
    uint64_t unmapped() const { return unmapped_; }
    uint64_t noInstrument() const { return noInstrument_; }
    uint64_t unsupported() const { return unsupported_; }

    void reset_counters() { routed_ = nonChannel_ = unmapped_ = noInstrument_ = unsupported_ = 0; }

  private:
    /** The instrument track `t`'s incoming events play, or −1 for "nothing to play them on". */
    int input_instrument(int t) const {
        int id = tracks_ ? tracks_->current(static_cast<uint8_t>(t)) : INSTRUMENT_NONE;
        if (id < 0) id = fallback_;
        if (id < 0 || static_cast<size_t>(id) >= project_->instruments.size()) return -1;
        return id;
    }

    /** Fill one record. False = this message has no bus form (aftertouch), so nothing is written. */
    bool build(const MidiInMessage& msg, int64_t frame, uint8_t track, int instrument, Event& ev) const {
        ev = Event{};
        ev.frame = frame;
        ev.track = track;

        // ⚠️ The note-off test comes FIRST: a note-on with velocity 0 is a note-off, and reaching the
        // note-on arm with it would raise a voice at velocity 0 that nothing ever answers.
        if (msg.is_note_off()) {
            ev.instrument   = INSTRUMENT_NONE;      // track-scoped, like every note-off on the bus
            ev.type         = EV_NOTE_OFF;
            // ⭐ **NOTE_OFF_KEY, AND E1 DELIBERATELY DID NOT EMIT IT** — until E4 built the injection
            // path there was no consumer that read a third mode, and a mode nothing reads is a mode
            // nothing can be wrong about. Now there is: §4.1's rule (ADSR/TRIG release, looping
            // soft-kill, **one-shot plays out**) lives in `SamplerVoice::keyRelease`, and this is the
            // only emitter in the tree that asks for it. A KIL keeps meaning what it always meant.
            ev.noteOff.mode = NOTE_OFF_KEY;
            return true;
        }

        if (msg.is_note_on()) {
            const Instrument& ins = project_->instruments[static_cast<size_t>(instrument)];
            ev.instrument = static_cast<int16_t>(instrument);
            ev.type       = EV_NOTE_ON;

            NoteOnPayload& n = ev.noteOn;
            n.note = msg.data1;
            // ⚠️ **THE SCHEDULER'S WIRING, COPIED RATHER THAN REASONED ABOUT** (scheduler.h:754/848):
            // velocity = the 0-127 byte, velGain = (v/127)² (the velocity CURVE), volGain = the
            // instrument's own volume. B5's velocity bug was a hand-built payload whose fields meant
            // something else to the consumer that read them; a live key has a real velocity byte and a
            // real V column equivalent, so it can and does use the sequencer's exact arrangement — and
            // `midi_velocity` then reproduces the byte the keyboard sent, scaled by instrument volume.
            const float unit = static_cast<float>(msg.data2) / 127.0f;
            n.velocity    = static_cast<int8_t>(msg.data2);
            n.velGainBits = f32_bits(unit * unit);
            n.volGainBits = f32_bits(volume_of(ins));
            n.panBits     = f32_bits(pan_of(ins));
            // No phrase behind this note: no FX, no slice, no start offset, and the instrument's own
            // table. ⚠️ transpose is 0 DELIBERATELY — chain and song transpose position a phrase's
            // notes within a song, and a key that played a different pitch than the one pressed is not
            // a feature anyone has asked a tracker for.
            n.start = -1; n.slice = -1; n.tableId = -1; n.tableRow = -1;
            n.transpose = 0; n.pit = 0; n.arp = 0;
            n.pslOffBits = n.pslDurBits = n.pbnRateBits = n.vibSpdBits = n.vibDepBits = f32_bits(0.0f);
            return true;
        }

        ev.instrument = INSTRUMENT_NONE;   // everything below is track-scoped, as on the sequencer's bus

        if (msg.status == EV_CC) {
            ev.type = EV_CC;
            // A literal controller number, straight through — `resolve_cc_param` passes 0-127 unchanged,
            // so an incoming CC 10 moves pan on a sampler and rides the cable on an EXTERNAL instrument,
            // through the very same §6 map the `CCA` phrase command uses. The value widens 0-127 → 0-1
            // here because `to7bit` narrows it there, and those two are the only conversions that exist.
            ev.cc.param     = msg.data1;
            ev.cc.valueBits = f32_bits(static_cast<float>(msg.data2) / 127.0f);
            return true;
        }

        if (msg.status == EV_PROGRAM) {
            ev.type = EV_PROGRAM;
            ev.program.program = msg.data1;
            return true;
        }

        if (msg.status == EV_PITCH_BEND) {
            ev.type = EV_PITCH_BEND;
            // LSB first on the wire, 14 bits, centre 0x2000 — the exact value `pitch_bend_event`
            // re-splits on its way out, so a bend arriving on an EXTERNAL instrument leaves unchanged.
            ev.pitchBend.value14 =
                static_cast<uint16_t>((static_cast<int>(msg.data2) << 7) | static_cast<int>(msg.data1));
            return true;
        }

        // 0xA0 poly key pressure, 0xD0 channel pressure: no bus form and no engine that could use one.
        // An explicit arm rather than a default, because a `default:` cannot tell "dropped deliberately"
        // from "forgotten" — the same reason engine_consumer.h spells out MPG and MPB.
        return false;
    }

    // VolumeUtils.hexToFloat, spelled out rather than pulled in: scheduler.h is the sequencer and this
    // file has no business including it. (model.h is where the shared arithmetic lives — `note_to_midi`
    // and `chain_is_empty` both moved there for this reason — and hex_to_float follows in the same
    // commit, so these two call it.)
    static float volume_of(const Instrument& ins) { return hex_to_float(ins.volume); }
    static float pan_of(const Instrument& ins) { return hex_to_float(ins.pan); }

    const Project*          project_  = nullptr;
    const TrackInstruments* tracks_   = nullptr;
    int                     fallback_ = -1;

    uint64_t routed_ = 0, nonChannel_ = 0, unmapped_ = 0, noInstrument_ = 0, unsupported_ = 0;
};

// ─── Where a drained, parsed, routed message goes (E2) ──────────────────────────────────────────

/**
 * Told about every message the host drained, with the bus records it produced.
 *
 * ⚠️ **THE APP's THREAD, NOT THE BACKEND's** — this is called from `SongcoreHost::poll()`, which is the
 * frame loop, and everything a backend's own thread does ends at `MidiInQueue::on_bytes`. That is the
 * whole reason the queue exists, and it is why an implementation of this may do anything it likes
 * (print, allocate, call the engine) where an `IMidiInSink` may not.
 *
 * `count` may be 0: a message that routed nowhere is still a message that ARRIVED, and telling the two
 * apart is the difference between "the cable is dead" and "no track is listening on that channel" —
 * two problems with completely different fixes, which is the same argument as the router's four
 * counters. E2's implementation is the shell's console; E4's is the injection into the engine.
 */
struct IMidiInObserver {
    virtual ~IMidiInObserver() = default;
    virtual void on_midi_in(const MidiInMessage& msg, const Event* events, int count) = 0;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_IN_H
