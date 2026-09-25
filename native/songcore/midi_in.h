#ifndef POCKETTRACKER_SONGCORE_MIDI_IN_H
#define POCKETTRACKER_SONGCORE_MIDI_IN_H

// ─── MIDI IN — bytes from a cable become bus events (MIDI plan phase E, §5) ──────────────────────
//
// The mirror image of midi_out.h, and deliberately shaped like it: `IMidiIn` is the platform seam,
// everything above it is platform-free, and the one routing question ("whose note is this?") is
// answered by ONE rule that every consumer asks rather than by each consumer's private test.
//
// Four objects, and the split is the point — each is the answer to a different kind of question:
//
//   • `MidiInQueue`  — WHO OWNS THE THREAD. A backend receives bytes on a thread it does not choose
//     (winmm calls back on its own; ALSA blocks in `read`; Android delivers on a binder thread), so
//     the bytes are parked in one lock-free ring and drained where the app likes. Written ONCE, above
//     the seam, for the reason midi-out-base.h states: three backends each carrying their own ring is
//     three rings that diverge in one of the copies.
//   • `MidiParser`   — THE PROTOCOL, and nothing else. Running status, real-time bytes arriving in the
//     middle of another message, SysEx, orphan data bytes. It reports the wire FAITHFULLY and makes no
//     policy decisions at all — including the note-on-velocity-0 convention, which is a `MidiInMessage`
//     predicate below rather than a rewrite here, so that every future reader of a message gets the
//     same answer without remembering to.
//   • `MidiInputRouter` — THE POLICY. Which track does channel 5 drive, which instrument is that
//     track's, and what does a bus record for it look like. It reads a `MidiRoute` — the routing
//     facts of the project as a flat struct — never the project itself.
//   • `MidiInPipeline` — THE DRAIN. Queue → parser → the knob gate → router, run by whoever owns the
//     consuming thread, plus the ring that carries what it saw back to the UI thread.
//
// ⚠️⚠️ **THE DRAIN RUNS ON THE AUDIO THREAD.** The engine calls `MidiInPipeline::run` at the top of
// every block, so a key is heard in the block after its bytes arrive, with no frame-loop poll and no
// lead-in between them. Everything the drain touches is therefore real-time safe: the byte ring is
// lock-free, the routing facts arrive as a POD snapshot the UI thread publishes (`MidiRoute`), and
// everything with an opinion that needs the project — the mappings, MIDI thru, the counters a screen
// shows — happens on the UI thread from the observer ring, one poll later. A host with no engine
// runs the same `run` from its own poll; it is the same code either way.
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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "event.h"
#include "midi_map.h"   // the knob gate: ctl_ch_covers, which CCs a mapping claims
#include "model.h"
#include "router.h"     // TrackInstruments — the shared "whose track is this?" rule

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
 * A bounded byte ring between the backend's thread and the thread that drains it.
 *
 * Bytes and not messages, deliberately: the backend has no parser and must not need one (ALSA hands
 * over whatever a `read` returned, which can split a message down the middle), and a byte ring
 * RESYNCS by construction — the parser ignores data bytes until the next status byte, so even a
 * mangled stream costs at most one message.
 *
 * ⚠️ **ONE PRODUCER, ONE CONSUMER, NO LOCK.** The consumer is the audio thread, and a mutex the
 * backend's thread could be preempted while holding is a wait the audio thread cannot afford. The
 * producer owns `tail_`, the consumer owns `head_`, and each only ever reads the other's. That is the
 * whole of the contract: a port has one delivering thread, and `clear()` belongs to the consumer.
 *
 * ⚠️ **OVERFLOW IS COUNTED, NEVER SILENT** — the same rule as `MidiClock::dropped_ticks()`: a component
 * that quietly swallows work cannot be told from one that had nothing to do. `dropped()` is printed
 * beside the verdict wherever this is reported.
 */
class MidiInQueue : public IMidiInSink {
  public:
    // 1 KB = ~341 three-byte messages, drained every audio block (a few ms). A MIDI 1.0 cable carries
    // about 1 040 three-byte messages a second, so the ring holds a third of a second of a cable
    // running flat out. The counter exists for the case this reasoning is wrong.
    static constexpr int CAPACITY = 1024;   // a power of two: the slot is `index & MASK`

    /** The producer's side. Drops the NEWEST when full, keeping every complete message already
     *  received — dropping the oldest would throw away note-ONs whose note-offs are still coming. */
    void on_bytes(const uint8_t* data, int len) override {
        if (!data || len <= 0) return;
        const uint32_t head = head_.load(std::memory_order_acquire);
        uint32_t       tail = tail_.load(std::memory_order_relaxed);
        for (int i = 0; i < len; ++i) {
            if (tail - head >= static_cast<uint32_t>(CAPACITY)) {
                dropped_.fetch_add(static_cast<uint64_t>(len - i), std::memory_order_relaxed);
                break;
            }
            buf_[tail & MASK] = data[i];
            ++tail;
        }
        tail_.store(tail, std::memory_order_release);
    }

    /** The consumer's side: move up to `max` bytes into `out`. Returns how many. */
    int drain(uint8_t* out, int max) {
        if (!out || max <= 0) return 0;
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        uint32_t       head = head_.load(std::memory_order_relaxed);
        int n = 0;
        while (head != tail && n < max) { out[n++] = buf_[head & MASK]; ++head; }
        head_.store(head, std::memory_order_release);
        return n;
    }

    /** Bytes lost to a full ring, ever. Nonzero means the drain is not keeping up. */
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    int pending() const {
        return static_cast<int>(tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire));
    }

    /** ⚠️ The CONSUMER's call — it moves the consumer's own index. Anyone else asks the consumer. */
    void clear() { head_.store(tail_.load(std::memory_order_acquire), std::memory_order_release); }

    /** Where the producer has written up to, as a mark for `discard_up_to`. Any thread. */
    uint32_t written() const { return tail_.load(std::memory_order_acquire); }

    /** The CONSUMER's: drop everything written before `mark`, keep what came after it. */
    void discard_up_to(uint32_t mark) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        if (static_cast<int32_t>(mark - head) > 0) head_.store(mark, std::memory_order_release);
    }

  private:
    static constexpr uint32_t MASK = CAPACITY - 1;
    static_assert((CAPACITY & MASK) == 0, "the ring's capacity must be a power of two");

    uint8_t               buf_[CAPACITY] = {0};
    std::atomic<uint32_t> head_{0};      // consumer's
    std::atomic<uint32_t> tail_{0};      // producer's
    std::atomic<uint64_t> dropped_{0};
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
        if (status_ == 0) { orphans_.fetch_add(1, std::memory_order_relaxed); return false; }   // no running status to belong to

        pending_[pendingCount_++] = b;
        if (pendingCount_ < expect_) return false;

        emit();
        pendingCount_ = 0;
        if (status_ >= 0xF0) status_ = 0;     // System Common is one-shot; channel status runs on
        return true;
    }

    const MidiInMessage& message() const { return msg_; }

    /** Data bytes seen with no status byte to belong to. A resync, or a bug — nonzero is worth a look. */
    uint64_t orphan_bytes() const { return orphans_.load(std::memory_order_relaxed); }

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
    std::atomic<uint64_t> orphans_{0};   // read by the UI's counters, written by the drain
};

// ─── The route — the routing facts, flat, for a thread that cannot read the project ─────────────

/** What the router needs to know about one instrument. */
struct MidiRouteInstrument {
    float   volume   = 1.0f;   // hex_to_float(ins.volume) — baked into a note-on, as the sequencer does
    float   pan      = 0.5f;
    uint8_t external = 0;      // routes to the cable, not to a voice
};

/**
 * The routing facts of a project as one POD block: which channel each track hears, which instrument
 * answers for it (the IN INS row, what the sequencer last played there, and the UI's current one, in
 * that order), what each instrument bakes into a note, and which CCs the mappings claim.
 *
 * ⚠️ **THIS IS THE ONLY VIEW OF THE PROJECT THE DRAIN EVER SEES.** The project holds strings and
 * vectors the UI thread reallocates; this is built from it on the UI thread (`build_midi_route`) and
 * published whole, so the audio thread routes against a consistent picture that is at most one poll
 * old. Anything a future routing rule needs goes in HERE, not in a pointer to the project.
 */
struct MidiRoute {
    int8_t   channel[POOL_TRACKS];         // the track's IN CH; -1 = not listening
    int16_t  rowInstrument[POOL_TRACKS];   // the IN INS row; -1 = unset
    int16_t  learned[POOL_TRACKS];         // what the sequencer last played on the track; -1 = nothing yet
    int16_t  fallback;                     // the instrument the UI is showing; -1 = none
    int16_t  instrumentCount;              // ids at or past this resolve to nothing
    int8_t   controlChannel;               // the CTL CH row: 0-15, MIDI_CTL_CH_ALL, or -1 for none
    uint8_t  learnArmed;                   // R is held: a knob on the control channel names, never drives
    uint64_t claimed[2];                   // bit per controller number: a mapping would drive it
    MidiRouteInstrument ins[POOL_INSTRUMENTS];

    bool claims(uint8_t controller) const {
        return controller < 128 && ((claimed[controller >> 6] >> (controller & 63)) & 1u) != 0;
    }
};

/**
 * Build the route from the live project. UI thread.
 *
 * `claimed` is the set of controllers `apply_mapped_cc` would drive right now — same predicate, so a
 * CC the drain hands to the mappings is one the mappings will take. A destination that has gone
 * (the instrument slot cleared, the track out of range) is not claimed, and its knob falls through to
 * the track exactly as it did before the mapping existed.
 */
inline MidiRoute build_midi_route(const Project& p, const TrackInstruments& learned, int fallback,
                                  int controlChannel, bool learnArmed) {
    MidiRoute r{};
    for (int t = 0; t < POOL_TRACKS; ++t) {
        const size_t i = static_cast<size_t>(t);
        r.channel[t]       = i < p.midiInputChannels.size()    ? static_cast<int8_t>(p.midiInputChannels[i])     : -1;
        r.rowInstrument[t] = i < p.midiInputInstruments.size() ? static_cast<int16_t>(p.midiInputInstruments[i]) : -1;
        r.learned[t]       = learned.current(static_cast<uint8_t>(t));
    }
    r.fallback        = static_cast<int16_t>(fallback);
    r.instrumentCount = static_cast<int16_t>(p.instruments.size() < static_cast<size_t>(POOL_INSTRUMENTS)
                                                 ? p.instruments.size() : static_cast<size_t>(POOL_INSTRUMENTS));
    r.controlChannel  = static_cast<int8_t>(controlChannel);
    r.learnArmed      = learnArmed ? 1 : 0;
    for (int i = 0; i < r.instrumentCount; ++i) {
        const Instrument& ins = p.instruments[static_cast<size_t>(i)];
        r.ins[i].volume   = hex_to_float(ins.volume);
        r.ins[i].pan      = hex_to_float(ins.pan);
        r.ins[i].external = instrument_routes_external(ins) ? 1 : 0;
    }
    for (const MidiMapping& m : p.midiMappings) {
        if (m.controller > 127) continue;
        if (!map_dest(m.dest) || !map_dest_present(p, m)) continue;
        r.claimed[m.controller >> 6] |= (1ull << (m.controller & 63));
    }
    return r;
}

/**
 * One writer publishes a `MidiRoute`, one reader copies it out without ever waiting.
 *
 * A sequence lock: the writer bumps `seq_` to odd, writes, bumps to even; the reader copies and
 * keeps the copy only if `seq_` was even and unchanged across it. The words are atomics so the
 * overlap is defined; a reader that lands on a write in progress keeps the route it already had,
 * which is at most one poll old.
 */
class MidiRoutePublisher {
  public:
    /** UI thread. */
    void publish(const MidiRoute& r) {
        uint64_t words[WORDS] = {0};
        std::memcpy(words, &r, sizeof r);
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < WORDS; ++i) words_[i].store(words[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }

    /** The consumer. True when `out` now holds a route newer than `seen` (which is updated). */
    bool read(MidiRoute& out, uint32_t& seen) const {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) continue;            // a write is in progress: try once more, then keep ours
            if (s1 == seen) return false;     // nothing new
            uint64_t words[WORDS];
            for (size_t i = 0; i < WORDS; ++i) words[i] = words_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) != s1) continue;
            std::memcpy(&out, words, sizeof out);
            seen = s1;
            return true;
        }
        return false;
    }

    bool published() const { return seq_.load(std::memory_order_acquire) != 0; }

  private:
    static constexpr size_t WORDS = (sizeof(MidiRoute) + 7) / 8;
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint64_t> words_[WORDS] = {};
};

// ─── The router — channel to track, track to instrument, message to bus record ──────────────────

/**
 * Turns a parsed message into the bus records it means, for whichever tracks are listening.
 *
 * ⚠️ **THE INSTRUMENT IS NOT IN THE MAP, AND THAT IS THE RATIFIED DATA MODEL** (§7): a track's input
 * entry is a CHANNEL and nothing else, so "which instrument does this key play?" has to be answered
 * from somewhere. The route answers it in the order the IN INS row, then *the track's current
 * instrument* — `TrackInstruments`, the SAME object both bus consumers use to decide who owns a
 * track-scoped event, copied into the route by the UI thread — then the instrument the UI is showing.
 *
 * ⚠️ **With a fallback, and the fallback is what makes the feature work at all on the first try.**
 * `TrackInstruments` learns from note-ons, so on a stopped song — or a track that has not played yet —
 * it knows nothing, and a keyboard would be silent with everything correctly configured. That is the
 * failure mode §B2 wrote down the hard way: *a feature whose "not configured" state is
 * indistinguishable from its "broken" state will be reported as broken.* So the host supplies the
 * instrument the UI is currently showing — the same one the A-button preview auditions — and a live key
 * plays what you are looking at until the sequencer says otherwise.
 *
 * Fan-out is real: several tracks may name the same input channel, so one message can produce up to
 * `POOL_TRACKS` records.
 *
 * ⚠️⚠️ **A CHORD IS SHARED BETWEEN TRACKS THAT PLAY THE SAME INSTRUMENT, AND COPIED TO TRACKS THAT DO
 * NOT.** A tracker track is one voice, so eight tracks listening to one channel used to play the same
 * key eight times over — unison, not a chord. Now the listening tracks are grouped by the instrument
 * they resolve to: each note-on takes ONE track out of its group (the one idle longest; if all are
 * holding a key, the oldest of them is stolen), and each group gets its own copy of the note. So
 * eight tracks with one instrument is an eight-voice piano, four plus four is a two-voice layer of two
 * sounds, and one track alone is exactly what it always was. The grouping is DERIVED from the
 * instrument row — there is no mode to set, and nothing to get wrong except which instrument a track
 * plays, which is the thing the screen already shows.
 *
 * ⚠️ **NOTE-OFF FOLLOWS THE KEY, NOT THE CHANNEL**: the track a key was handed to is remembered, and
 * its release is the only one that goes out. A release for a key that was stolen finds no owner and
 * is dropped — the note it would have ended is already over.
 *
 * ⚠️ Everything that is NOT a note — CC, program change, pitch bend — still reaches EVERY listening
 * track. Those are channel-wide on the wire and a mod wheel that moved only one voice of a chord
 * would be a bug on any synth.
 *
 * ⚠️ Runs on the drain's thread. The counters are atomics because the MIDI screen reads them from
 * the UI thread; everything else here is the drain's alone.
 */
class MidiInputRouter {
  public:
    // One event per track, at most: a message never produces two records for one track.
    static constexpr int MAX_EVENTS = POOL_TRACKS;

    MidiInputRouter() { release_all_keys(); }

    /** The routing facts. The pointer must stay valid between calls; the drain owns the copy. */
    void set_route(const MidiRoute* r) { route_ = r; }

    /**
     * Route one message onto the bus at `frame`. Returns how many records were written to `out`.
     *
     * `frame` is the caller's: the drain passes the first frame of the block it is running in, so a
     * live key sounds at the top of the block after its bytes arrived.
     */
    int route(const MidiInMessage& msg, int64_t frame, Event* out, int maxOut) {
        if (!route_ || !out || maxOut <= 0) return 0;

        // Real time and System Common: the protocol has them, this phase does not. SYNC IN is
        // §9-deferred and it is the only thing that would read them.
        if (!msg.is_channel()) { bump(nonChannel_); return 0; }

        int n = 0;
        bool anyTrack = false;

        // ⚠️ THE NOTE-OFF ARM COMES FIRST, for the reason `build` states: a note-on at velocity 0 IS a
        // note-off, and it has to find the track holding that key rather than take a new one.
        if (msg.is_note_off()) {
            for (int t = 0; t < POOL_TRACKS && n < maxOut; ++t) {
                if (!listens(t, msg.channel)) continue;
                anyTrack = true;
                if (held_[t] != static_cast<int>(msg.data1)) continue;
                held_[t] = -1;
                // ⚠️ STAMPED ON THE WAY OUT AS WELL AS THE WAY IN, and that is the whole of "idle
                // longest": a track stamped only when it TOOK a key would look older the more
                // recently it let one go, so the next note would land on the tail still ringing.
                idle_[t] = ++clock_;
                if (build(msg, frame, static_cast<uint8_t>(t), INSTRUMENT_NONE, out[n])) { ++n; bump(routed_); }
            }
            if (!anyTrack) bump(unmapped_);
            return n;
        }

        if (msg.is_note_on()) {
            // One note per INSTRUMENT among the listening tracks. `served` is the instruments this
            // message has already been given to, so the second track of a group is skipped rather
            // than handed a second copy.
            int served[POOL_TRACKS];
            int servedCount = 0;
            for (int t = 0; t < POOL_TRACKS && n < maxOut; ++t) {
                if (!listens(t, msg.channel)) continue;
                anyTrack = true;

                const int instrument = input_instrument(t);
                if (instrument < 0) { bump(noInstrument_); continue; }

                bool already = false;
                for (int i = 0; i < servedCount; ++i) already = already || (served[i] == instrument);
                if (already) continue;
                served[servedCount++] = instrument;

                const int target = take_track(msg.channel, instrument);
                if (target < 0) continue;   // cannot happen: `t` itself is a candidate
                held_[target]    = static_cast<int>(msg.data1);
                heldIns_[target] = static_cast<int16_t>(instrument);
                idle_[target]    = ++clock_;
                if (build(msg, frame, static_cast<uint8_t>(target), instrument, out[n])) { ++n; bump(routed_); }
                else                                                                     bump(unsupported_);
            }
            if (!anyTrack) bump(unmapped_);
            return n;
        }

        // CC, program change, pitch bend: channel-wide, so every listening track gets one.
        for (int t = 0; t < POOL_TRACKS && n < maxOut; ++t) {
            if (!listens(t, msg.channel)) continue;
            anyTrack = true;

            const int instrument = input_instrument(t);
            if (instrument < 0) { bump(noInstrument_); continue; }

            if (build(msg, frame, static_cast<uint8_t>(t), instrument, out[n])) {
                ++n;
                bump(routed_);
            } else {
                bump(unsupported_);
            }
        }

        if (!anyTrack) bump(unmapped_);
        return n;
    }

    /**
     * Every key this router is holding is released — the tracks are free again.
     *
     * ⚠️ Call it wherever the notes themselves are silenced (a PANIC, a stop, a project load), or the
     * allocator goes on believing tracks are busy and starts stealing from the first note.
     */
    void release_all_keys() {
        for (int t = 0; t < POOL_TRACKS; ++t) { held_[t] = -1; heldIns_[t] = INSTRUMENT_NONE; }
    }

    /** The instrument a track-scoped record on `track` is for: the key it holds, else what a key
     *  would resolve to now. `INSTRUMENT_NONE` when nothing answers. */
    int16_t instrument_of(uint8_t track) const {
        if (track >= POOL_TRACKS) return INSTRUMENT_NONE;
        if (held_[track] >= 0 && heldIns_[track] >= 0) return heldIns_[track];
        const int id = input_instrument(track);
        return id < 0 ? INSTRUMENT_NONE : static_cast<int16_t>(id);
    }

    // ── counters: every path that produces no event says which one it was ────────────────────────
    // ⭐ A component whose correct behaviour is silence cannot be told from one that never ran. Four
    // separate reasons, because "nothing happened" has four completely different fixes: turn on SYNC
    // (nonChannel), map a track (unmapped), pick an instrument (noInstrument), or nothing at all
    // (unsupported — aftertouch, which this engine has no form for).
    uint64_t routed() const { return routed_.load(std::memory_order_relaxed); }
    uint64_t nonChannel() const { return nonChannel_.load(std::memory_order_relaxed); }
    uint64_t unmapped() const { return unmapped_.load(std::memory_order_relaxed); }
    uint64_t noInstrument() const { return noInstrument_.load(std::memory_order_relaxed); }
    uint64_t unsupported() const { return unsupported_.load(std::memory_order_relaxed); }

    void reset_counters() {
        for (std::atomic<uint64_t>* c : {&routed_, &nonChannel_, &unmapped_, &noInstrument_, &unsupported_})
            c->store(0, std::memory_order_relaxed);
    }

  private:
    static void bump(std::atomic<uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }

    /** Does track `t` listen to this channel at all? */
    bool listens(int t, uint8_t channel) const {
        return route_->channel[t] == static_cast<int8_t>(channel);
    }

    /**
     * The instrument track `t`'s incoming events play, or −1 for "nothing to play them on".
     *
     * ⚠️ **THE ROW WINS WHERE IT IS SET**, and where it is not the old rule stands: the instrument the
     * track last played, then the one the INSTRUMENT screen is showing. The fallback is what makes a
     * keyboard work before anything is configured; the row is what makes the answer sayable out loud.
     */
    int input_instrument(int t) const {
        int id = route_->rowInstrument[t];
        if (id < 0) id = route_->learned[t];
        if (id < 0) id = route_->fallback;
        if (id < 0 || id >= route_->instrumentCount) return -1;
        return id;
    }

    /**
     * Which track of a group takes the next key: the one idle longest, else the oldest note in it.
     *
     * ⚠️ **LEAST RECENTLY USED, NOT "THE FIRST FREE ONE"** — a track that has just let a key go is
     * still ringing its release, and handing it the next note of a run would cut every tail off. The
     * counter is a plain sequence number stamped at both ends of a key's life: it never wraps in any
     * session a person will play.
     */
    int take_track(uint8_t channel, int instrument) {
        int free = -1, oldest = -1;
        for (int t = 0; t < POOL_TRACKS; ++t) {
            if (!listens(t, channel) || input_instrument(t) != instrument) continue;
            if (held_[t] < 0) { if (free   < 0 || idle_[t] < idle_[free])   free   = t; }
            else              { if (oldest < 0 || idle_[t] < idle_[oldest]) oldest = t; }
        }
        return free >= 0 ? free : oldest;
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
            // A live key let go of is NOTE_OFF_KEY, and the engine treats it by §4.1's rule
            // (ADSR/TRIG release, looping soft-kill, a one-shot plays out — `SamplerVoice::keyRelease`).
            // This is the only emitter in the tree that asks for it; a KIL keeps meaning what it meant.
            ev.noteOff.mode = NOTE_OFF_KEY;
            return true;
        }

        if (msg.is_note_on()) {
            if (instrument < 0 || instrument >= POOL_INSTRUMENTS) return false;   // cannot happen: routed above
            const MidiRouteInstrument& ins = route_->ins[instrument];
            ev.instrument = static_cast<int16_t>(instrument);
            ev.type       = EV_NOTE_ON;

            NoteOnPayload& n = ev.noteOn;
            n.note = msg.data1;
            // ⚠️ **THE SCHEDULER'S WIRING, COPIED RATHER THAN REASONED ABOUT** (scheduler.h emit_note):
            // velocity = the 0-127 byte, velGain = (v/127)² (the velocity CURVE), volGain = the
            // instrument's own volume. A hand-built payload whose fields meant something else to the
            // consumer that read them is a bug this file has had once; a live key has a real velocity
            // byte and a real V column equivalent, so it uses the sequencer's exact arrangement — and
            // `midi_velocity` then reproduces the byte the keyboard sent, scaled by instrument volume.
            const float unit = static_cast<float>(msg.data2) / 127.0f;
            n.velocity    = static_cast<int8_t>(msg.data2);
            n.velGainBits = f32_bits(unit * unit);
            n.volGainBits = f32_bits(ins.volume);
            n.panBits     = f32_bits(ins.pan);
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

    const MidiRoute* route_ = nullptr;

    // The allocator's whole state: which key each track is holding (−1 = none), which instrument it
    // took it for, and when it last took one. ⚠️ Per TRACK and not per group — a track's group can
    // change under it (the row is editable while a key is down), and a key that is being held has to
    // be releasable whatever the screen says afterwards.
    int      held_[POOL_TRACKS];
    int16_t  heldIns_[POOL_TRACKS];
    uint64_t idle_[POOL_TRACKS] = {};
    uint64_t clock_             = 0;

    std::atomic<uint64_t> routed_{0}, nonChannel_{0}, unmapped_{0}, noInstrument_{0}, unsupported_{0};
};

// ─── What the drain saw, carried back to the UI thread ───────────────────────────────────────────

/**
 * Told about every message the drain handled, with the bus records it produced.
 *
 * ⚠️ **THE UI THREAD, NOT THE DRAIN's** — `SongcoreHost::poll()` calls this from the observer ring,
 * one poll after the audio thread routed the message. That is why an implementation of this may do
 * anything it likes (print, allocate, call the engine) where an `IMidiInSink` may not.
 *
 * `count` may be 0: a message that routed nowhere is still a message that ARRIVED, and telling the two
 * apart is the difference between "the cable is dead" and "no track is listening on that channel" —
 * two problems with completely different fixes, which is the same argument as the router's four
 * counters. The shell's console is one implementation; the tools' recorders are the others.
 */
struct IMidiInObserver {
    virtual ~IMidiInObserver() = default;
    virtual void on_midi_in(const MidiInMessage& msg, const Event* events, int count) = 0;
};

/** One handled message, as the drain hands it back. */
struct MidiInSeen {
    enum Kind : uint8_t {
        ROUTED = 0,   // `events[0..count)` went to the engine; the UI owes thru and the bookkeeping
        MAPPED = 1,   // a CC a mapping claims: the UI applies it to the song, nothing was routed
        LEARN  = 2,   // a CC on the control channel while R was held: it names, it does not drive
    };
    MidiInMessage msg;
    Kind          kind  = ROUTED;
    uint8_t       count = 0;
    Event         events[MidiInputRouter::MAX_EVENTS];
};

/**
 * Queue → parser → the knob gate → router, and where the records go.
 *
 * Owns nothing that names a thread. `run(frame, apply)` is called by the CONSUMER of the byte ring —
 * the engine, at the top of every live block, or a host with no engine from its own poll — and
 * `apply` is what the consumer does with a record (the engine queues it for this block; a bare host
 * passes nullptr). Everything else crosses back to the UI thread through `pop_seen`.
 *
 * ⚠️⚠️ **REAL-TIME SAFE BY CONSTRUCTION, and the list of what that took is the contract:**
 *   • the byte ring and the seen ring are lock-free single-producer/single-consumer;
 *   • the route is a POD copy, read through the sequence lock and never waited for;
 *   • the counters are relaxed atomics;
 *   • a reset (port closed) and a release-all (transport stopped) are REQUESTS the drain honours at
 *     the top of its next run, because the parser and the allocator are the drain's own state and
 *     nobody else may touch them. A host with no engine services them itself, at once.
 *
 * ⚠️ The knob gate is decided HERE, from the route's `claimed` set, because one knob must not do two
 * jobs: a CC that drives a mapping must not also move the track's instrument, and the drain is the
 * only place that can withhold it from the router. The mapping itself is applied on the UI thread,
 * which owns the song.
 */
class MidiInPipeline {
  public:
    struct Apply {
        virtual ~Apply() = default;
        /** One bus record from a live key, at the drain's frame. `external` is the note-on's
         *  instrument routing to the cable; a voice must not be raised for it. */
        virtual void apply(const Event& ev, bool external) = 0;
    };

    static constexpr int SEEN_CAPACITY = 128;   // messages between two UI polls; a power of two

    // ── the producer's side (the backend's thread) ───────────────────────────────────────────────
    MidiInQueue& sink() { return queue_; }

    // ── the UI thread ────────────────────────────────────────────────────────────────────────────
    void publish_route(const MidiRoute& r) { publisher_.publish(r); }

    /**
     * Forget everything mid-flight — the bytes parked so far, the parser's half message, the keys the
     * allocator believes are held. Honoured at the top of the drain's next run.
     *
     * ⚠️ "So far" is a MARK taken now: a port closed and another opened before the drain's next
     * block delivers its first bytes behind the mark, and those are kept and parsed fresh. Without
     * the mark the new port's first key would be thrown away with the old port's leftovers.
     */
    void request_reset() {
        resetMark_.store(queue_.written(), std::memory_order_relaxed);
        requests_.fetch_or(REQ_RESET, std::memory_order_release);
    }
    /** Every held key released — the tracks are free again. Honoured at the drain's next run. */
    void request_release_keys() { requests_.fetch_or(REQ_RELEASE, std::memory_order_release); }

    /** The next handled message, oldest first. False when there is none. */
    bool pop_seen(MidiInSeen& out) {
        const uint32_t tail = seenTail_.load(std::memory_order_acquire);
        const uint32_t head = seenHead_.load(std::memory_order_relaxed);
        if (head == tail) return false;
        out = seen_[head & (SEEN_CAPACITY - 1)];
        seenHead_.store(head + 1, std::memory_order_release);
        return true;
    }
    bool has_seen() const {
        return seenHead_.load(std::memory_order_acquire) != seenTail_.load(std::memory_order_acquire);
    }

    // ── the consumer ─────────────────────────────────────────────────────────────────────────────

    /**
     * Drain → parse → gate → route → `apply`, every record stamped `frame`.
     *
     * Returns the number of bus records produced. `apply` may be null (no engine): the records are
     * still routed, counted and handed back, which is what lets a host tool check the policy without
     * a voice to raise.
     */
    int run(int64_t frame, Apply* apply) {
        service_requests();
        if (publisher_.read(route_, routeSeen_)) router_.set_route(&route_);
        if (!publisher_.published()) { queue_.clear(); return 0; }   // nothing to route against yet

        // The buffer is the ring's whole capacity, so ONE drain always empties it: a second pass could
        // only pick up bytes that arrived during this one, and those belong to the next block anyway.
        uint8_t buf[MidiInQueue::CAPACITY];
        const int n = queue_.drain(buf, static_cast<int>(sizeof buf));
        if (n <= 0) return 0;
        bytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

        int total = 0;
        for (int i = 0; i < n; ++i) {
            if (!parser_.feed(buf[i])) continue;
            messages_.fetch_add(1, std::memory_order_relaxed);
            const MidiInMessage& m = parser_.message();

            MidiInSeen seen{};
            seen.msg = m;

            // ⚠️⚠️ **A CC IS OFFERED TO THE MAPPINGS FIRST, AND ONE THAT DRIVES SOMETHING IS CONSUMED.**
            // ⭐ **CLAIMED, NOT RESERVED, AND THAT IS WHAT LETS `ALL` BE THE DEFAULT**: a CC no mapping
            // claims falls straight through to the router and behaves exactly as it did before the
            // feature existed. ⚠️ **WHILE LEARN IS ARMED THE KNOB NAMES AND DOES NOT DRIVE** — holding
            // `R` to point a knob at a new parameter must not also sweep whatever it already drove.
            if (m.status == EV_CC && ctl_ch_covers(route_.controlChannel, static_cast<int>(m.channel))) {
                if (route_.learnArmed)        { seen.kind = MidiInSeen::LEARN;  push_seen(seen); continue; }
                if (route_.claims(m.data1))   { seen.kind = MidiInSeen::MAPPED; push_seen(seen); continue; }
            }

            const int k = router_.route(m, frame, seen.events, MidiInputRouter::MAX_EVENTS);
            seen.count = static_cast<uint8_t>(k);
            total += k;
            injected_.fetch_add(static_cast<uint64_t>(k), std::memory_order_relaxed);
            for (int j = 0; j < k; ++j) {
                const Event& ev = seen.events[j];
                const bool external =
                    ev.type == EV_NOTE_ON && ev.instrument >= 0 && ev.instrument < route_.instrumentCount &&
                    route_.ins[ev.instrument].external != 0;
                if (apply) apply->apply(ev, external);
            }
            // ⚠️ Handed back even when `k == 0`: a message that routed nowhere still ARRIVED.
            push_seen(seen);
        }
        return total;
    }

    /** The consumer, while it is not live (an export owns the engine): bytes are dropped rather
     *  than saved up to fire as a burst when the stream comes back. Counted as dropped. */
    void discard() {
        service_requests();
        const int n = queue_.pending();
        if (n > 0) {
            queue_.clear();
            discarded_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        }
    }

    /** What a host with no engine does on the UI thread, since it is the consumer there. */
    void service_requests() {
        const uint32_t r = requests_.exchange(0, std::memory_order_acquire);
        if (r & REQ_RESET) {
            queue_.discard_up_to(resetMark_.load(std::memory_order_relaxed));
            parser_.reset();
            router_.release_all_keys();
        }
        if (r & REQ_RELEASE) { router_.release_all_keys(); }
    }

    // ── counters: any thread, relaxed ────────────────────────────────────────────────────────────
    uint64_t bytes() const { return bytes_.load(std::memory_order_relaxed); }
    uint64_t messages() const { return messages_.load(std::memory_order_relaxed); }
    /** Records handed to `apply` (or routed, with none). */
    uint64_t injected() const { return injected_.load(std::memory_order_relaxed); }
    /** Bytes thrown away while the engine was not live. */
    uint64_t discarded() const { return discarded_.load(std::memory_order_relaxed); }
    /** Handled messages the UI thread never saw — its ring was full. Nonzero means thru and the
     *  counters missed something; the sound did not. */
    uint64_t seen_dropped() const { return seenDropped_.load(std::memory_order_relaxed); }

    const MidiInputRouter& router() const { return router_; }
    const MidiParser&      parser() const { return parser_; }
    const MidiInQueue&     queue()  const { return queue_; }

  private:
    static constexpr uint32_t REQ_RESET = 1, REQ_RELEASE = 2;

    void push_seen(const MidiInSeen& s) {
        const uint32_t head = seenHead_.load(std::memory_order_acquire);
        const uint32_t tail = seenTail_.load(std::memory_order_relaxed);
        if (tail - head >= static_cast<uint32_t>(SEEN_CAPACITY)) {
            seenDropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        seen_[tail & (SEEN_CAPACITY - 1)] = s;
        seenTail_.store(tail + 1, std::memory_order_release);
    }

    MidiInQueue        queue_;
    MidiParser         parser_;
    MidiInputRouter    router_;
    MidiRoutePublisher publisher_;
    MidiRoute          route_{};        // the drain's copy
    uint32_t           routeSeen_ = 0;
    std::atomic<uint32_t> requests_{0};
    std::atomic<uint32_t> resetMark_{0};

    // ⚠️ On the heap: inline, the ring is ~83 KB and the host lives on the stack (Windows gives 1 MB).
    std::unique_ptr<MidiInSeen[]> seen_{new MidiInSeen[SEEN_CAPACITY]};
    std::atomic<uint32_t> seenHead_{0};   // the UI's
    std::atomic<uint32_t> seenTail_{0};   // the drain's

    std::atomic<uint64_t> bytes_{0}, messages_{0}, injected_{0}, discarded_{0}, seenDropped_{0};
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_IN_H
